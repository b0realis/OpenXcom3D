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
 * FirstPersonViewTerrain.cpp: the battlescape as voxels.
 *
 * Each tile part's voxel model is built from its LOFT shapes (the ones line of fire uses) and
 * coloured from its isometric sprite; the sides of crafts the art never shows are mirrored from
 * the sides it does; doors swing and slide; the ground runs on outside the map; and every frame
 * a snapshot of every tile is taken for the ray caster to read.
 */
#include "FirstPersonView.h"
#include <memory>
#include <algorithm>
#include <chrono>
#include <array>
#include <bitset>
#include <climits>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <SDL.h>
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Palette.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleTerrain.h"
#include "../Mod/RuleUfo.h"
#include "../Savegame/Ufo.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Base.h"
#include "../Savegame/Craft.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Battlescape/Position.h"
#include "RuleFirstPerson.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/// When a voxel projects onto a transparent pixel of its sprite (the art left a gap there), the
/// nearest drawn pixel up or down the same sprite column gives its colour instead. These are
/// the row offsets tried, nearest first, in sprite pixels.
const int SpriteNudges[] = { 0, -1, 1, -2, 2, -3, 3, -4, 4, -6, 6, -8, 8, -12, 12 };

/// A patch of solid black in a UFO part's art is a hole in the picture, not its colour, when at
/// least this many black pixels touch each other, or when it touches the art's outline (see
/// findHollowPixels).
const int HollowBlackPixels = 40;

/**
 * Finds the pixels of a UFO part's sprite that are a hole in the art rather than its colour:
 * patches of solid black that are big (HollowBlackPixels or more black pixels touching side by
 * side) or touch the picture's outline (a transparent pixel or the sprite's edge beside them).
 * The curved hull of X-COM's UFOs is drawn with black triangles where the isometric camera
 * would look into it, and outlined in black along its edges; in 3D the voxels behind them
 * showed as black stripes and wedges on the inner walls. Small black specks inside the picture
 * (the shadows in a texture) are left alone.
 * @param sprite The sprite.
 * @param palette Its palette, to tell black (darker than 14 of 255 to the eye).
 * @param hollow Set for each pixel (row by row, the sprite's width across) in such a patch.
 * @return True if any pixel is.
 */
bool findHollowPixels(const Surface *sprite, const SDL_Color *palette, std::vector<char> &hollow)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	hollow.assign(width * height, 0);
	auto isBlack = [&](int x, int y)
	{
		const Uint8 color = sprite->getPixel(x, y);
		return color && palette[color].r * 299 + palette[color].g * 587 + palette[color].b * 114 < 14000;
	};
	std::vector<char> visited(width * height, 0);
	std::vector<int> patch, toVisit;
	bool found = false;
	for (int start = 0; start < width * height; ++start)
	{
		if (visited[start] || !isBlack(start % width, start / width)) continue;

		// The patch: every black pixel joined to this one, side by side, and whether it touches
		// the outline.
		patch.clear();
		toVisit.assign(1, start);
		visited[start] = 1;
		bool onOutline = false;
		while (!toVisit.empty())
		{
			const int pixel = toVisit.back();
			toVisit.pop_back();
			patch.push_back(pixel);
			const int x = pixel % width, y = pixel / width;
			const int sides[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
			for (const auto &side : sides)
			{
				if (side[0] < 0 || side[1] < 0 || side[0] >= width || side[1] >= height || !sprite->getPixel(side[0], side[1]))
				{
					onOutline = true;
					continue;
				}
				const int sideIndex = side[1] * width + side[0];
				if (!visited[sideIndex] && isBlack(side[0], side[1]))
				{
					visited[sideIndex] = 1;
					toVisit.push_back(sideIndex);
				}
			}
		}

		// Big enough, or on the outline: a hole.
		if ((int)patch.size() >= HollowBlackPixels || onOutline)
		{
			for (const int pixel : patch) hollow[pixel] = 1;
			found = true;
		}
	}
	return found;
}

/**
 * Paints the voxels that took their colour from a hole in the art (see findHollowPixels) like
 * the voxels around them: pass by pass, each takes the colour of a neighbour with a real colour
 * (one of its six; the ones in the same layer first), so the paint spreads in from the hole's
 * edges. Any with no such voxel within reach take the sprite's most common colour.
 * @param model The model, its voxels coloured (the hollow ones too, black).
 * @param hollowVoxels The hollow voxels, as (z * 16 + y) * 16 + x; used up.
 * @param fallbackColor The sprite's most common colour.
 */
void fillHollowVoxels(FirstPersonView::VoxelModel &model, std::vector<int> &hollowVoxels, Uint8 fallbackColor)
{
	std::vector<char> unpainted(24 * 16 * 16, 0);
	for (const int voxel : hollowVoxels) unpainted[voxel] = 1;
	const int sides[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	std::vector<std::pair<int, Uint8>> paintedNow;
	for (int pass = 0; pass < 48 && !hollowVoxels.empty(); ++pass)
	{
		// Each hollow voxel next to a painted one takes its colour (all at once, after the pass,
		// so the paint moves one voxel a pass).
		paintedNow.clear();
		for (const int voxel : hollowVoxels)
		{
			const int x = voxel % 16, y = (voxel / 16) % 16, z = voxel / 256;
			for (const auto &side : sides)
			{
				const int sideX = x + side[0], sideY = y + side[1], sideZ = z + side[2];
				if (sideX < 0 || sideY < 0 || sideZ < 0 || sideX > 15 || sideY > 15 || sideZ > 23) continue;
				const Uint8 sideColor = model.color[sideZ][sideY][sideX];
				if (sideColor && !unpainted[(sideZ * 16 + sideY) * 16 + sideX])
				{
					paintedNow.push_back({ voxel, sideColor });
					break;
				}
			}
		}
		if (paintedNow.empty()) break;
		for (const auto &painted : paintedNow)
		{
			model.color[painted.first / 256][(painted.first / 16) % 16][painted.first % 16] = painted.second;
			unpainted[painted.first] = 0;
		}
		hollowVoxels.erase(std::remove_if(hollowVoxels.begin(), hollowVoxels.end(), [&](int voxel) { return !unpainted[voxel]; }), hollowVoxels.end());
	}
	for (const int voxel : hollowVoxels) model.color[voxel / 256][(voxel / 16) % 16][voxel % 16] = fallbackColor;
}

/**
 * Empties a voxel model, ready to be built: no solid voxels, no glass, no glow, no colours for
 * the hidden sides, and an inverted bounding box (min 255, max 0) that the first solid voxel
 * will set.
 * @param model The model to clear.
 */
void clearModel(FirstPersonView::VoxelModel &model)
{
	// Every colour 0 (empty) and every glass and hole bit off.
	std::fill(&model.color[0][0][0], &model.color[0][0][0] + sizeof(model.color), (Uint8)0);
	std::fill(&model.glass[0][0], &model.glass[0][0] + 24 * 16, (Uint16)0);
	std::fill(&model.lookOnly[0][0], &model.lookOnly[0][0] + 24 * 16, (Uint16)0);
	std::fill(&model.clear[0][0], &model.clear[0][0] + 24 * 16, (Uint16)0);

	model.pulse = false;
	// No light source found yet.
	model.glow[0] = model.glow[1] = model.glow[2] = 0.0f;
	model.glowCount = 0;
	model.glowColor = 0;
	model.glowMinZ = 0;

	// No colours yet for the sides the isometric art never shows.
	std::fill(&model.plan[0][0], &model.plan[0][0] + 16 * 16, (Uint8)0);
	std::fill(&model.east[0][0], &model.east[0][0] + 24 * 16, (Uint8)0);
	std::fill(&model.south[0][0], &model.south[0][0] + 24 * 16, (Uint8)0);

	// Not a UFO part's steady model (see buildUfoModel): no lights marked, no smooth shading.
	model.lit = nullptr;
	model.exposed = nullptr;
	model.pulseKind = 0;

	// Empty, not a ghost, with a bounding box that any solid voxel will shrink onto.
	model.empty = true;
	model.ghostly = false;
	model.solidGhost = false;
	model.wayThrough = false;
	model.water = false;
	model.liquidHeight = 0;
	model.artFill = false;
	model.openingGlass = false;
	model.cropBlock = 0;
	model.ghostKind = 0;
	model.reliefRises = false;
	model.foliage = false;
	model.ghost = nullptr;
	for (int axis = 0; axis < 3; ++axis)
	{
		model.min[axis] = 255;
		model.max[axis] = 0;
	}
}

/**
 * Recolours the top of a thin plate (a floor, a roof) that the art draws as a raised slab. On
 * such a sprite the plate's top, projected where its voxels really are, lands on the slab's
 * side edge instead of its top face (the Skyranger's roof plates; a band of cabin trim on every
 * tile). So the top layer's projection is lifted until its outline meets the top of what the
 * sprite draws, and the top layer takes its colours again from there.
 *
 * The geometry is the same as in buildModel: the voxel (x, y, z) of the tile lands on sprite
 * pixel (16 + x - y, 24 + (x + y) / 2 - z + yOffset).
 * @param model The model, already built from its LOFT templates; only its top layer changes.
 * @param sprite The tile part's isometric sprite (32x40 pixels).
 * @param yOffset How many pixels higher than usual the map draws this sprite (MapData::getYOffset).
 * @param nudgeCount How many of SpriteNudges to try when a pixel is transparent.
 */
void fitPlateTopToSprite(FirstPersonView::VoxelModel &model, const Surface *sprite, int yOffset, int nudgeCount)
{
	// For each sprite column, the first row from the top that the art draws (-1: nothing in it).
	int spriteTop[32];
	for (int spriteX = 0; spriteX < 32; ++spriteX)
	{
		spriteTop[spriteX] = -1;
		for (int spriteY = 0; spriteX < sprite->getWidth() && spriteY < sprite->getHeight(); ++spriteY)
		{
			if (sprite->getPixel(spriteX, spriteY)) { spriteTop[spriteX] = spriteY; break; }
		}
	}

	// How badly the plate's top fits the sprite when lifted by some pixels: for each sprite
	// column, the highest row the top layer's voxels land on (left in plateOutline, used below)
	// against the first row the art draws there; the average gap over the columns where both
	// exist, times 16 to keep some precision in integers (INT_MAX: no column to compare).
	int plateOutline[32];
	auto misfit = [&](int lift)
	{
		// The outline of the top layer, projected and lifted.
		std::fill(plateOutline, plateOutline + 32, INT_MAX);
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const int topLayer = model.max[2];
				if (!model.color[topLayer][y][x]) continue;
				const int spriteX = std::clamp(16 + x - y, 0, 31);
				plateOutline[spriteX] = std::min(plateOutline[spriteX], 24 + (x + y) / 2 - topLayer + yOffset - lift);
			}
		// The average gap between that outline and the drawn top, column by column.
		int totalGap = 0, columnCount = 0;
		for (int spriteX = 0; spriteX < 32; ++spriteX)
		{
			if (plateOutline[spriteX] == INT_MAX || spriteTop[spriteX] < 0) continue;
			totalGap += std::abs(spriteTop[spriteX] - plateOutline[spriteX]);
			++columnCount;
		}
		return columnCount ? totalGap * 16 / columnCount : INT_MAX;
	};

	// Try lifting by 1 to 12 pixels and keep the lift that fits best.
	int bestLift = 0, bestMisfit = misfit(0);
	for (int lift = 1; lift <= 12; ++lift)
	{
		const int liftMisfit = misfit(lift);
		if (liftMisfit < bestMisfit) { bestLift = lift; bestMisfit = liftMisfit; }
	}

	// Only a lift that fits at least twice as well as none is believed: then the art really
	// draws a raised slab.
	if (bestLift > 0 && bestMisfit * 2 < misfit(0)) // (misfit(0) leaves the unlifted outline behind)
	{
		// Each column of the sprite is lifted by its own amount, the gap between the unlifted
		// outline and the drawn top there (a curved roof is drawn higher on one side); a column
		// without both takes the best overall lift.
		int columnLift[32];
		for (int spriteX = 0; spriteX < 32; ++spriteX)
		{
			columnLift[spriteX] = plateOutline[spriteX] != INT_MAX && spriteTop[spriteX] >= 0
				? std::clamp(plateOutline[spriteX] - spriteTop[spriteX], 0, 12) : bestLift;
		}

		// Recolour every solid voxel of the top layer from the lifted spot in the sprite (the
		// nearest drawn pixel up or down the column if it is transparent; unchanged if none).
		const int topLayer = model.max[2];
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				if (!model.color[topLayer][y][x]) continue;
				const int spriteX = std::clamp(16 + x - y, 0, sprite->getWidth() - 1);
				const int spriteY = 24 + (x + y) / 2 - topLayer + yOffset - columnLift[std::min(spriteX, 31)];
				Uint8 color = 0;
				for (int nudge = 0; nudge < nudgeCount && !color; ++nudge) color = sprite->getPixel(spriteX, spriteY + SpriteNudges[nudge]);
				if (color) model.color[topLayer][y][x] = color;
			}
	}
}

/**
 * The colour of a palette nearest to an RGB colour (0..255 per channel), weighting the channels
 * by how bright they look to the eye. Colour 0 is transparent, so it is never picked.
 */
Uint8 nearestColor(const SDL_Color *palette, const float *rgb)
{
	int best = 1;
	float bestDistance = 1e9f;
	for (int index = 1; index < 256; ++index)
	{
		const float dr = palette[index].r - rgb[0], dg = palette[index].g - rgb[1], db = palette[index].b - rgb[2];
		const float distance = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = index;
		}
	}
	return (Uint8)best;
}

/**
 * Works out the colours of the sides the isometric art never shows. The art looks at the map
 * from above and from the south-east, so it shows only tops and the faces looking east and
 * south. Seen in 3D, an underside, a plate's ceiling, and the faces looking west and north
 * would otherwise show whatever colour the projection happened to put through them. Fills in
 * model.plan (each column as seen from above), model.ceiling (a thin plate's underside),
 * model.east and model.south (each row as seen from the east or the south).
 * @param model The model, with its voxels already coloured.
 * @param sprite The sprite it was coloured from, for its palette.
 */
void colorHiddenSides(FirstPersonView::VoxelModel &model, const Surface *sprite)
{
	// The underside (a ceiling seen from below, the bottom of a table) shows the pattern seen
	// from above, column by column: the colour of the highest solid voxel in each column. Its
	// own voxels would show the top's shading smeared down the projection in diagonal bands.
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			for (int z = 23; z >= 0; --z)
			{
				if (model.color[z][y][x]) { model.plan[y][x] = model.color[z][y][x]; break; }
			}

	// A plate's underside is a ceiling the art never shows (and its top often runs onto a trim
	// drawn along its edge), so it gets one plain colour: of the colours its top shows, the one
	// nearest their average. A plate is a model with all its voxels in the bottom 4 layers.
	model.ceiling = 0;
	const SDL_Palette *palette = const_cast<Surface*>(sprite)->getSurface()->format->palette;
	if (!model.empty && model.max[2] <= 3 && palette && palette->ncolors >= 256)
	{
		// Sum up the red, green and blue of the top's colours.
		int colorSum[3] = {}, topCount = 0;
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = model.plan[y][x];
				if (!color) continue;
				colorSum[0] += palette->colors[color].r; colorSum[1] += palette->colors[color].g; colorSum[2] += palette->colors[color].b;
				++topCount;
			}
		// Pick the top colour nearest the average (red, green and blue differences added up).
		// dr, dg and db are each channel's difference from the average, times topCount, so
		// the average itself never needs a division.
		int bestDistance = INT_MAX;
		for (int y = 0; y < 16 && topCount; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = model.plan[y][x];
				if (!color) continue;
				const int dr = palette->colors[color].r * topCount - colorSum[0],
					dg = palette->colors[color].g * topCount - colorSum[1],
					db = palette->colors[color].b * topCount - colorSum[2];
				const int distance = (std::abs(dr) + std::abs(dg) + std::abs(db)) / topCount;
				if (distance < bestDistance) { bestDistance = distance; model.ceiling = color; }
			}
	}

	// Likewise the sides the art never shows (facing west or north) show what the same row of
	// voxels looks like from the side it does show (east or south): the colour of the row's
	// easternmost (southernmost) solid voxel. A wall then has its texture on both sides.
	for (int z = 0; z < 24; ++z)
		for (int row = 0; row < 16; ++row)
		{
			// The row running east-west at y = row, seen from the east.
			for (int x = 15; x >= 0; --x)
			{
				if (model.color[z][row][x]) { model.east[z][row] = model.color[z][row][x]; break; }
			}
			// The row running north-south at x = row, seen from the south.
			for (int y = 15; y >= 0; --y)
			{
				if (model.color[z][y][row]) { model.south[z][row] = model.color[z][y][row]; break; }
			}
		}
}

/**
 * Finds the lit part of a model that may be a light source: its brightest shades in the top
 * third of the object (a lamp's bulb sits on top). In the game's palette each group of 16
 * colours runs from bright to dark, so a "bright shade" is one of the first four of its group.
 * Fills in model.glowMinZ (the lowest layer that may shine), model.glow (the centre of the
 * bright voxels, in voxels within the tile), model.glowCount and model.glowColor. Whether the
 * part really is a light source is the MapData's business (see snapshotTile).
 * @param model The model, with its voxels already coloured.
 */
void findGlow(FirstPersonView::VoxelModel &model)
{
	if (model.empty)
	{
		return;
	}
	// Only the top third of the object may shine.
	model.glowMinZ = (Uint8)(model.max[2] - (model.max[2] - model.min[2]) / 3);

	// Add up the centres of the bright voxels there; the first one found gives the glow colour.
	for (int z = model.glowMinZ; z <= model.max[2]; ++z)
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = model.color[z][y][x];
				if (!color || (color & 15) > 3) continue;
				model.glow[0] += x + 0.5f;
				model.glow[1] += y + 0.5f;
				model.glow[2] += z + 0.5f;
				if (!model.glowCount++) model.glowColor = color;
			}

	// Their average is the centre of the glow.
	if (model.glowCount)
	{
		for (float &coordinate : model.glow) coordinate /= model.glowCount;
	}
}

/**
 * Does a wall the mod lists as a window really look like one? The list goes by data set name
 * and record number, and another mod may use the same data set name for records of its own, so
 * the number could point at a plain wall, whose pixels in the glass colours would then turn
 * see-through. A window is a wall (west or north) that is a door (a glass door), or whose shape
 * is thinner somewhere between its bottom and top layers than at its bottom (an opening, or a
 * pane thinner than the wall).
 * @param mapData The record.
 * @param loftData The game's LOFT templates (16 rows of 16 voxels each).
 * @return True if it may have glass.
 */
bool looksLikeWindow(const MapData *mapData, const std::vector<Uint16> &loftData)
{
	if (mapData->getObjectType() != O_WESTWALL && mapData->getObjectType() != O_NORTHWALL)
	{
		return false;
	}
	if (mapData->isDoor())
	{
		return true;
	}
	// The solid voxels in each of the 12 layers (each LOFT template covers two voxel layers).
	int solid[12] = {};
	int topLayer = -1;
	for (int layer = 0; layer < 12; ++layer)
	{
		const size_t loftStart = mapData->getLoftID(layer) * 16;
		for (int row = 0; row < 16 && loftStart + 16 <= loftData.size(); ++row)
		{
			solid[layer] += std::bitset<16>(loftData[loftStart + row]).count();
		}
		if (solid[layer]) topLayer = layer;
	}
	// Thinner than the bottom somewhere in between.
	for (int layer = 1; layer < topLayer; ++layer)
	{
		if (solid[layer] < solid[0]) return true;
	}
	return false;
}

/**
 * Finds the holes in a part's art: transparent pixels the drawn ones close in all round, which
 * no transparent path joins to the sprite's edge (a glass door's panes, the gaps in a fence, a
 * chain-link mesh's every other pixel). On the map one sees through them what lies behind,
 * though the game's voxels there are solid; the 3D view sees through the voxels behind them too
 * (a player's report: the gas station's glass doors were see-through on the map, opaque in 3D).
 * @param sprite The part's sprite frame.
 * @param holes Gets one entry per pixel, row by row: 1 for a hole.
 * @return True if the art has any hole.
 */
bool findArtHoles(const Surface *sprite, std::vector<char> &holes)
{
	// Every transparent pixel reachable from the edge without crossing a drawn one is outside;
	// the transparent ones left are holes.
	const int width = sprite->getWidth(), height = sprite->getHeight();
	std::vector<char> outside(width * height, 0);
	std::vector<int> toVisit;
	for (int x = 0; x < width; ++x) { toVisit.push_back(x); toVisit.push_back((height - 1) * width + x); }
	for (int y = 0; y < height; ++y) { toVisit.push_back(y * width); toVisit.push_back(y * width + width - 1); }
	while (!toVisit.empty())
	{
		const int pixel = toVisit.back();
		toVisit.pop_back();
		const int x = pixel % width, y = pixel / width;
		if (outside[pixel] || sprite->getPixel(x, y)) continue;
		outside[pixel] = 1;
		if (x > 0) toVisit.push_back(pixel - 1);
		if (x < width - 1) toVisit.push_back(pixel + 1);
		if (y > 0) toVisit.push_back(pixel - width);
		if (y < height - 1) toVisit.push_back(pixel + width);
	}
	holes.assign(width * height, 0);
	bool found = false;
	for (int pixel = 0; pixel < width * height; ++pixel)
	{
		if (!outside[pixel] && !sprite->getPixel(pixel % width, pixel / width))
		{
			holes[pixel] = 1;
			found = true;
		}
	}
	return found;
}

/**
 * A wall's shape (the game's voxels) seen across it, on its plane: for each cell along the wall
 * by its height (16 by 24), whether any of its rows across has a voxel there; the rows across
 * with voxels at all (x for a west wall, y for a north wall); and how high it reaches.
 */
struct WallPlane
{
	bool covered[Position::TileZ][Position::TileXY]; // [z][along]
	Uint16 rows = 0;      // bit per row across with a voxel
	int coveredCells = 0; // how many cells are covered
	int top = -1;         // the highest height with a covered cell; -1 none
	/// The share of the plane covered, 0..1.
	float cover() const { return coveredCells / (float)(Position::TileZ * Position::TileXY); }
	/// How many rows across it fills (its thickness).
	int thickness() const { return std::bitset<16>(rows).count(); }
};

/**
 * A wall's plane (see WallPlane), from the LOFT templates of its layers.
 * @param mapData The wall's record.
 * @param loftData The LOFT templates.
 * @return Its plane.
 */
WallPlane wallPlane(const MapData *mapData, const std::vector<Uint16> &loftData)
{
	const bool westWall = mapData->getObjectType() == O_WESTWALL;
	WallPlane plane;
	for (int z = 0; z < Position::TileZ; ++z)
	{
		const size_t loftStart = mapData->getLoftID(z / 2) * 16;
		for (int along = 0; along < Position::TileXY; ++along)
		{
			bool any = false;
			for (int across = 0; across < Position::TileXY && loftStart + 16 <= loftData.size(); ++across)
			{
				const int x = westWall ? across : along, y = westWall ? along : across;
				if ((loftData[loftStart + y] >> (15 - x)) & 1)
				{
					any = true;
					plane.rows |= (Uint16)(1 << across);
				}
			}
			plane.covered[z][along] = any;
			if (any)
			{
				++plane.coveredCells;
				plane.top = z;
			}
		}
	}
	return plane;
}

/**
 * Behind the art's holes, how much of a wall's plane its shape covers: of the plane's cells whose
 * place in the art (in any of the wall's rows across, as buildModel lays voxels on the art) is a
 * hole, how many are covered (the game's own glass there: sight passes, shots stop) and how many
 * open (an opening: shots pass, as a barn's window).
 * @param mapData The wall's record.
 * @param sprite Its sprite frame.
 * @param holes Its art's holes (see findArtHoles).
 * @param plane Its plane (see wallPlane).
 * @param covered Gets the covered cells behind the holes.
 * @param open Gets the open ones.
 */
void shapeBehindHoles(const MapData *mapData, const Surface *sprite, const std::vector<char> &holes, const WallPlane &plane, int &covered, int &open)
{
	const bool westWall = mapData->getObjectType() == O_WESTWALL;
	const int width = sprite->getWidth(), height = sprite->getHeight(), yOffset = mapData->getYOffset();
	covered = open = 0;
	for (int z = 0; z < Position::TileZ; ++z)
	{
		for (int along = 0; along < Position::TileXY; ++along)
		{
			bool behindHole = false;
			for (int across = 0; across < Position::TileXY && !behindHole; ++across)
			{
				if (!((plane.rows >> across) & 1)) continue;
				const int x = westWall ? across : along, y = westWall ? along : across;
				const int spriteX = std::clamp(16 + x - y, 0, width - 1);
				const int spriteY = 24 + (x + y) / 2 - z + yOffset;
				behindHole = spriteY >= 0 && spriteY < height && holes[spriteY * width + spriteX];
			}
			if (behindHole) ++(plane.covered[z][along] ? covered : open);
		}
	}
}

/**
 * The glass of a pane with holes in its art, as the game's voxels behind it show it: in each
 * column of the art, from the top to the bottom of the window's holes there (the patches of at
 * least GlassSmallPaneMin pixels; a frame's tiny gaps don't count), the holes and what's drawn
 * among them (a glass panel's glints and streaks, a crossbar), not the art above or below them
 * (a shop's wall round its small window, which a pane taken as all inside the art's edge made
 * glass: the owner, "in iso there are windows and in 3d the whole wall is seethrough?").
 * @param sprite Its sprite frame.
 * @param holes Its art's holes (see findArtHoles).
 * @param glass Gets one entry per pixel, row by row: 1 for glass.
 */
void paneHoleSpans(const Surface *sprite, const std::vector<char> &holes, std::vector<char> &glass)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	glass.assign(width * height, 0);
	if (holes.empty())
	{
		return;
	}
	// The window's holes: the patches big enough, joined together.
	std::vector<char> windowHole(width * height, 0), seen(width * height, 0);
	for (int start = 0; start < width * height; ++start)
	{
		if (!holes[start] || seen[start]) continue;
		std::vector<int> patch = { start }, toVisit = { start };
		seen[start] = 1;
		while (!toVisit.empty())
		{
			const int pixel = toVisit.back();
			toVisit.pop_back();
			const int x = pixel % width, y = pixel / width;
			const int neighbours[4] = { x > 0 ? pixel - 1 : -1, x < width - 1 ? pixel + 1 : -1, y > 0 ? pixel - width : -1, y < height - 1 ? pixel + width : -1 };
			for (int next : neighbours)
			{
				if (next >= 0 && holes[next] && !seen[next])
				{
					seen[next] = 1;
					toVisit.push_back(next);
					patch.push_back(next);
				}
			}
		}
		if ((int)patch.size() >= GlassSmallPaneMin)
		{
			for (int pixel : patch) windowHole[pixel] = 1;
		}
	}
	// Each column: from its top window hole to its bottom one, the art (hole or drawn) is glass.
	for (int x = 0; x < width; ++x)
	{
		int top = -1, bottom = -1;
		for (int y = 0; y < height; ++y)
		{
			if (!windowHole[y * width + x]) continue;
			if (top < 0) top = y;
			bottom = y;
		}
		for (int y = top; top >= 0 && y <= bottom; ++y)
		{
			if (holes[y * width + x] || sprite->getPixel(x, y)) glass[y * width + x] = 1;
		}
	}
}

/**
 * Is most of a sprite's art pale bluish glass (see GlassPaintedPaleMin): its drawn pixels' colours
 * light in every channel, no warmer than a touch, and nearly grey or lilac?
 * @param sprite The sprite (palette colours, 0 transparent).
 * @param palette The battlescape's palette.
 * @return True if it is.
 */
bool paintedGlass(const Surface *sprite, const SDL_Color *palette)
{
	if (!palette)
	{
		return false;
	}
	int drawn = 0, pale = 0;
	for (int y = 0; y < sprite->getHeight(); ++y)
	{
		for (int x = 0; x < sprite->getWidth(); ++x)
		{
			const Uint8 color = sprite->getPixel(x, y);
			if (!color) continue;
			++drawn;
			const SDL_Color &rgb = palette[color];
			const int lowest = std::min({ (int)rgb.r, (int)rgb.g, (int)rgb.b }), highest = std::max({ (int)rgb.r, (int)rgb.g, (int)rgb.b });
			if (lowest >= GlassPaleMinChannel && rgb.b + GlassPaleBlueSlack >= rgb.r && highest - lowest <= GlassPaleSpread) ++pale;
		}
	}
	return drawn > 0 && pale >= GlassPaintedPaleMin * drawn;
}

/**
 * Is a wall a pane of glass (a shop's window, a glass wall)? The game lets sight through it (it
 * doesn't block the view, though its voxels stop shots), and its art is a frame round a big
 * see-through area (holes of at least GlassPaneMinPixels joined together; a fence's gaps and a
 * chain-link mesh are smaller), or round a window of small panes (a house's window split by
 * bars: patches of at least GlassSmallPaneMin pixels adding up to GlassPaneMinPixels, with at most
 * GlassMeshPatchesMax smaller ones, a mesh having many more, on a wall whose shape covers at least
 * GlassWallCoverMin of its plane, not a post or a railing; the owner, after GitHub issue #75:
 * "glass by the game's rules"). Its pane is everything inside the frame: the holes and what the
 * art draws among them (glints, streaks, bars), not the frame itself (the drawn pixels next to the
 * transparent ones outside it, or on the sprite's edge). Or its glass is painted (no holes) on a
 * solid shape the game sees through: most of its art pale bluish glass, its shape a whole wall,
 * thin (see GlassPaintedPaleMin; X-Com Files' panels and glass doors, the market's: the map shows
 * the room behind them; not an ice cave's rock). A player, GitHub issue #75: X-Com Files' shop
 * windows were opaque; the owner: "just a smooth see-through pane".
 * Only a window's (the owner: "barn windows in farm are just openings as before without glass", then
 * "railings open"): a big pane's wall covers at least GlassBigPaneCoverMin of its plane (not barbed
 * wire, a blasted breach, an arch); a window's wall reaches GlassWallTopMin up (a deck railing's
 * gaps are no panes); and one hole with no bars across it is glass only where the game's own shape
 * stands across most of it (a kiosk's glass, a glass door: sight passes, shots stop), while one the
 * game leaves open is an opening, as the map and 1.09 show it (a barn's window, an alien base's).
 * @param mapData The record.
 * @param sprite Its sprite frame.
 * @param holes Its art's holes (see findArtHoles).
 * @param loftData The LOFT templates (the wall's shape).
 * @param palette The battlescape's palette (for painted glass's colours).
 * @param pane Gets one entry per pixel, row by row: 1 inside the pane.
 * @param painted Set true for painted glass (its whole pane is glass on the game's voxels; a pane
 *        with holes is glass there only behind its holes, see buildModel).
 * @return True if it's a pane of glass.
 */
bool paneOfGlass(const MapData *mapData, const Surface *sprite, const std::vector<char> &holes, const std::vector<Uint16> &loftData, const SDL_Color *palette,
	std::vector<char> &pane, bool &painted)
{
	if ((mapData->getObjectType() != O_WESTWALL && mapData->getObjectType() != O_NORTHWALL) || mapData->getBlock(DT_NONE) != 0)
	{
		return false;
	}
	// The patches of holes joined together: the biggest, the small panes' total and how many
	// tiny ones (a mesh's).
	const int width = sprite->getWidth(), height = sprite->getHeight();
	std::vector<char> seen(width * height, 0);
	int biggest = 0, smallPanes = 0, tinyPatches = 0, paneCount = 0;
	for (int start = 0; start < width * height; ++start)
	{
		if (!holes[start] || seen[start]) continue;
		int size = 0;
		std::vector<int> toVisit = { start };
		seen[start] = 1;
		while (!toVisit.empty())
		{
			const int pixel = toVisit.back();
			toVisit.pop_back();
			++size;
			const int x = pixel % width, y = pixel / width;
			const int neighbours[4] = { x > 0 ? pixel - 1 : -1, x < width - 1 ? pixel + 1 : -1, y > 0 ? pixel - width : -1, y < height - 1 ? pixel + width : -1 };
			for (int next : neighbours)
			{
				if (next >= 0 && holes[next] && !seen[next])
				{
					seen[next] = 1;
					toVisit.push_back(next);
				}
			}
		}
		biggest = std::max(biggest, size);
		if (size >= GlassSmallPaneMin)
		{
			smallPanes += size;
			++paneCount;
		}
		else
		{
			++tinyPatches;
		}
	}
	const WallPlane plane = wallPlane(mapData, loftData);
	const bool bigPane = biggest >= GlassPaneMinPixels && plane.cover() >= GlassBigPaneCoverMin;
	const bool smallPaned = smallPanes >= GlassPaneMinPixels && tinyPatches <= GlassMeshPatchesMax && plane.cover() >= GlassWallCoverMin;
	painted = !bigPane && !smallPaned && biggest < GlassSmallPaneMin && plane.thickness() <= GlassThinRowsMax && paintedGlass(sprite, palette)
		&& plane.cover() >= GlassPaintedCoverMin;
	if (!bigPane && !smallPaned && !painted)
	{
		return false;
	}
	if (!painted)
	{
		// A railing's gaps: its wall stops short of the top, where a window has wall above it.
		if (plane.top < GlassWallTopMin)
		{
			return false;
		}
		// One hole the game leaves open: an opening.
		int covered = 0, open = 0;
		if (paneCount <= 1)
		{
			shapeBehindHoles(mapData, sprite, holes, plane, covered, open);
			if (covered < open)
			{
				return false;
			}
		}
	}
	// The pane: the holes, and the drawn pixels with no transparent pixel outside the frame next
	// to them (nor on the sprite's edge).
	pane.assign(width * height, 0);
	auto outsideAt = [&](int x, int y) { return x < 0 || y < 0 || x >= width || y >= height || (!sprite->getPixel(x, y) && !holes[y * width + x]); };
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const int pixel = y * width + x;
			if (holes[pixel])
			{
				pane[pixel] = 1;
			}
			else if (sprite->getPixel(x, y) && !outsideAt(x - 1, y) && !outsideAt(x + 1, y) && !outsideAt(x, y - 1) && !outsideAt(x, y + 1))
			{
				pane[pixel] = 1;
			}
		}
	}
	return true;
}

/**
 * Does the game give a record no voxels at all (every layer's LOFT template empty)? Shots and
 * sight pass through it, whatever the map draws.
 * @param mapData The record.
 * @param loftData The game's LOFT templates (16 rows of 16 voxels each).
 * @return True if it has no solid voxel.
 */
bool hasNoVoxels(const MapData *mapData, const std::vector<Uint16> &loftData)
{
	for (int layer = 0; layer < 12; ++layer)
	{
		const size_t loftStart = mapData->getLoftID(layer) * 16;
		for (int row = 0; row < 16 && loftStart + 16 <= loftData.size(); ++row)
		{
			if (loftData[loftStart + row]) return false;
		}
	}
	return true;
}

/**
 * Keeps a window's glass inside its panes. The art draws a wall's top edge, the sill and lintel
 * of a window and the ends of a wall in the same light colours as the glass, and seen through,
 * those would let the sky show through a wall in single pixels (looking down from a roof, say).
 * So a glass voxel stays glass only if the voxels above and below it are solid (not the top of
 * the wall, nor next to the window's opening) and it isn't at either end of the wall.
 * @param model The window's model, with its voxels and glass already filled in.
 * @param part The wall it is: O_WESTWALL (it runs north-south, along y) or O_NORTHWALL (along x).
 */
void keepGlassInPanes(FirstPersonView::VoxelModel &model, TilePart part)
{
	for (int z = 0; z < 24; ++z)
	{
		for (int y = 0; y < 16; ++y)
		{
			if (!model.glass[z][y]) continue;
			for (int x = 0; x < 16; ++x)
			{
				if (!(model.glass[z][y] & (1 << x))) continue;
				// Air above (the wall's top, or the opening over a sill) or below (the opening
				// under a lintel); below the bottom layer counts as solid ground.
				const bool airAbove = z == 23 || !model.color[z + 1][y][x];
				const bool airBelow = z > 0 && !model.color[z - 1][y][x];
				// An end of the wall: its first or last voxel along its length.
				const int along = part == O_WESTWALL ? y : x;
				if (airAbove || airBelow || along == 0 || along == 15)
				{
					model.glass[z][y] &= (Uint16)~(1 << x);
				}
			}
		}
	}
}

/**
 * Is a part's art a cage (a player: the bars round Cydonia's pyramid lifts should stand "on all
 * sides of tile, like a cage")? Bars standing on the floor's two back edges, as the isometric
 * art draws a fence round a tile (the front ones would hide what stands inside): an object whose
 * every drawn column ends at the back edges' line (within CageEdgeRows rows below it, none
 * above), at least CageMinColumns columns on each half of the sprite (the west edge's and the
 * north edge's).
 * @param sprite The part's sprite frame.
 * @param partType Which part of a tile it is (only an object can be one).
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @return True for a cage.
 */
bool cageArt(const Surface *sprite, int partType, int yOffset)
{
	if (partType != O_OBJECT || !sprite)
	{
		return false;
	}
	const int width = sprite->getWidth(), height = sprite->getHeight();
	int westColumns = 0, northColumns = 0;
	for (int spriteX = 0; spriteX < width; ++spriteX)
	{
		// The column's lowest drawn pixel, against where the back edges' floor line runs there.
		int bottom = -1;
		for (int spriteY = height - 1; spriteY >= 0 && bottom < 0; --spriteY)
		{
			if (sprite->getPixel(spriteX, spriteY)) bottom = spriteY;
		}
		if (bottom < 0)
		{
			continue;
		}
		const double backEdgeRow = 24 + std::abs(spriteX - 16) / 2.0 + yOffset;
		if (bottom < backEdgeRow - 0.5 || bottom > backEdgeRow + CageEdgeRows)
		{
			return false; // (something that doesn't stand on the back edges)
		}
		if (spriteX < 16) ++westColumns;
		else if (spriteX > 16) ++northColumns;
	}
	return westColumns >= CageMinColumns && northColumns >= CageMinColumns;
}

/**
 * Builds the ghost of a part the map draws but gives no voxels at all (setting "Parts without
 * shape"): a ladder, a lift's pad, a wall of laser beams, a lift's cage of bars. Its art, voxel
 * for voxel, on one plane of the tile (a cage on four), so that it shows where it stands:
 * - a west or north wall on the tile's west or north edge, where the art draws walls;
 * - a floor or an object whose art keeps low, inside the floor's diamond (a lift's pad), flat on
 *   the floor;
 * - one whose art rises above it (a ladder, drawn upright), upright, on the plane across the
 *   tile (running north-south or east-west) that shows the most of the art; ties go to the plane
 *   nearer the tile's middle, then to the north-south one;
 * - a cage (see cageArt): on the tile's west and north edges, where the art draws its bars, and
 *   the same again on the east and south edges, all round.
 * Each voxel takes the colour of the pixel it lands on (the projection buildModel uses), and
 * there's no voxel where that pixel is transparent, so a ladder keeps the gaps between its
 * rungs. On one plane no two voxels land on the same pixel. The model comes out ghostly: rays
 * see through it on some pixels, more of them the further away it is (see traceTile), and
 * nothing else takes it for solid.
 * @param model The empty model to build it in.
 * @param sprite The part's sprite frame.
 * @param partType Which part of a tile it is (O_FLOOR, O_WESTWALL, O_NORTHWALL, O_OBJECT).
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
/**
 * Tells whether art lies on the floor as buildGhost reads it: no more than a quarter of its drawn
 * pixels well above the floor's diamond (more than two rows above its top corner).
 * @param sprite The part's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @return True if it lies flat (and something of it is drawn).
 */
bool artLiesFlat(const Surface *sprite, int yOffset)
{
	int drawn = 0, rising = 0;
	for (int spriteY = 0; spriteY < sprite->getHeight(); ++spriteY)
	{
		for (int spriteX = 0; spriteX < sprite->getWidth(); ++spriteX)
		{
			if (!sprite->getPixel(spriteX, spriteY)) continue;
			++drawn;
			if (spriteY < 22 + yOffset) ++rising;
		}
	}
	return drawn && rising * 4 <= drawn;
}

/**
 * Tells whether an object without voxels is a body the art draws and the game lets no one walk
 * through (a console, a rack of machines: the owner on GitHub issue #150, "make an object from the
 * art"): not walkable, no lift, no door, and its art big, at least ArtBodyMinPixels drawn.
 * @param mapData The object's record.
 * @param sprite Its sprite frame.
 * @return True if it is built as a body from its art (see buildArtBody).
 */
bool artBody(const MapData *mapData, const Surface *sprite)
{
	if (mapData->getObjectType() != O_OBJECT || mapData->isGravLift() || mapData->isDoor() || mapData->isUFODoor()
		|| mapData->getTUCost(MT_WALK) < Pathfinding::INVALID_MOVE_COST)
	{
		return false;
	}
	int drawn = 0;
	for (int spriteY = 0; spriteY < sprite->getHeight(); ++spriteY)
		for (int spriteX = 0; spriteX < sprite->getWidth(); ++spriteX) drawn += sprite->getPixel(spriteX, spriteY) != 0;
	return drawn >= ArtBodyMinPixels;
}

/**
 * Builds an object without voxels as a body from its art (see artBody): every voxel of the tile
 * whose pixel the art draws (the projection buildModel uses), up to the height the art reaches (its
 * topmost drawn row, taken as the top's far corner, as a box's is), in that pixel's colour. From
 * where the map looks its outline and picture are the art's exactly; from elsewhere it is the
 * biggest body that looks so. A ghost the view draws solid (VoxelModel::solidGhost): the game's
 * shots and sight go through it, as the game gives it no voxels.
 * @param model The empty model to build it in.
 * @param sprite The object's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
void buildArtBody(FirstPersonView::VoxelModel &model, const Surface *sprite, int yOffset)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	int topRow = -1;
	for (int spriteY = 0; spriteY < height && topRow < 0; ++spriteY)
		for (int spriteX = 0; spriteX < width && topRow < 0; ++spriteX)
			if (sprite->getPixel(spriteX, spriteY)) topRow = spriteY;
	if (topRow < 0)
	{
		return;
	}
	const int top = std::clamp(24 + yOffset - topRow, 0, Position::TileZ - 1);
	for (int z = 0; z <= top; ++z)
	{
		for (int y = 0; y < Position::TileXY; ++y)
		{
			for (int x = 0; x < Position::TileXY; ++x)
			{
				const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
				const Uint8 color = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
				if (!color) continue;
				model.color[z][y][x] = color;
				model.empty = false;
				const int voxel[3] = { x, y, z };
				for (int axis = 0; axis < 3; ++axis)
				{
					model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
					model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
				}
			}
		}
	}
	model.ghostly = model.solidGhost = !model.empty;
}

void buildGhost(FirstPersonView::VoxelModel &model, const Surface *sprite, int partType, int yOffset)
{
	// The colour of a voxel's pixel; 0 (transparent) outside the sprite.
	const int width = sprite->getWidth(), height = sprite->getHeight();
	auto pixelOf = [&](int x, int y, int z) -> Uint8
	{
		const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
		return spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
	};

	// The art's drawn pixels, and how many of them are well above the floor's diamond (more than
	// two rows above its top corner, where nothing lying on the floor can show).
	int drawn = 0, rising = 0;
	for (int spriteY = 0; spriteY < height; ++spriteY)
	{
		for (int spriteX = 0; spriteX < width; ++spriteX)
		{
			if (!sprite->getPixel(spriteX, spriteY)) continue;
			++drawn;
			if (spriteY < 22 + yOffset) ++rising;
		}
	}
	if (!drawn)
	{
		return;
	}

	// A plane across the tile: axis 0 is x = offset (it runs north-south, along y), axis 1 is
	// y = offset (east-west, along x); -1 for flat on the floor.
	int axis = -1, offset = 0;
	auto planeVoxel = [](int planeAxis, int planeOffset, int along, int &x, int &y)
	{
		x = planeAxis == 0 ? planeOffset : along;
		y = planeAxis == 0 ? along : planeOffset;
	};
	if (partType == O_WESTWALL || partType == O_NORTHWALL)
	{
		axis = partType == O_WESTWALL ? 0 : 1;
	}
	else if (rising * 4 > drawn)
	{
		// Upright (more than a quarter of the art above the floor): the plane whose voxels land
		// on the most drawn pixels.
		int bestCount = -1;
		double bestFromMiddle = 0.0;
		for (int planeAxis = 0; planeAxis < 2; ++planeAxis)
		{
			for (int planeOffset = 0; planeOffset < 16; ++planeOffset)
			{
				int count = 0;
				for (int along = 0; along < 16; ++along)
				{
					for (int z = 0; z < 24; ++z)
					{
						int x, y;
						planeVoxel(planeAxis, planeOffset, along, x, y);
						if (pixelOf(x, y, z)) ++count;
					}
				}
				const double fromMiddle = std::abs(planeOffset - 7.5);
				if (count > bestCount || (count == bestCount && fromMiddle < bestFromMiddle))
				{
					bestCount = count;
					bestFromMiddle = fromMiddle;
					axis = planeAxis;
					offset = planeOffset;
				}
			}
		}
	}

	// An object's upright picture on the tile's west or north edge (a poster on the wall there) stands
	// GhostWallClearance voxels into the tile: on the edge itself it is inside the wall's own voxels,
	// and showed only once the wall was shot away (a player, GitHub issue #138). Its picture stays
	// the one the edge's plane has.
	const int shown = partType == O_OBJECT && axis >= 0 && offset < GhostWallClearance ? GhostWallClearance : offset;

	// The voxels: each where its pixel is drawn, with that pixel's colour.
	auto place = [&](int x, int y, int z)
	{
		const Uint8 color = pixelOf(x, y, z);
		if (!color) return;
		if (shown != offset) (axis == 0 ? x : y) = shown;
		model.color[z][y][x] = color;
		model.empty = false;
		const int voxel[3] = { x, y, z };
		for (int boxAxis = 0; boxAxis < 3; ++boxAxis)
		{
			model.min[boxAxis] = std::min<int>(model.min[boxAxis], voxel[boxAxis]);
			model.max[boxAxis] = std::max<int>(model.max[boxAxis], voxel[boxAxis]);
		}
	};
	if (cageArt(sprite, partType, yOffset))
	{
		// A cage: the west edge's bars (x = 0) again on the east edge, the north edge's (y = 0)
		// on the south edge.
		for (int along = 0; along < 16; ++along)
		{
			for (int z = 0; z < 24; ++z)
			{
				place(0, along, z);
				place(along, 0, z);
				const Uint8 west = pixelOf(0, along, z), north = pixelOf(along, 0, z);
				if (west) { model.color[z][along][15] = west; model.max[0] = 15; model.max[1] = std::max<int>(model.max[1], along); model.min[1] = std::min<int>(model.min[1], along); }
				if (north) { model.color[z][15][along] = north; model.max[1] = 15; model.max[0] = std::max<int>(model.max[0], along); model.min[0] = std::min<int>(model.min[0], along); }
			}
		}
	}
	else if (axis < 0)
	{
		// (Flat on the floor. A floor's diamond is drawn a pixel row lower than a voxel's pixel is read: a whole
		// diamond leaves the tile's north row of voxels, and every other voxel of its west edge, without a pixel.
		// Those take the pixel below theirs, so floor meets floor: a sea of such tiles had a dark line along every
		// tile's edge, what is under it showing through (a player, GitHub issue #194: "Water tiles don't completely
		// span across ... should cleanly tile, with 0 gaps in between").)
		for (int y = 0; y < 16; ++y)
		{
			for (int x = 0; x < 16; ++x)
			{
				if (pixelOf(x, y, 0)) { place(x, y, 0); continue; }
				const int spriteX = 16 + x - y, spriteY = 25 + (x + y) / 2 + yOffset;
				const Uint8 below = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
				if (!below) continue;
				model.color[0][y][x] = below;
				model.empty = false;
				model.min[0] = std::min<int>(model.min[0], x); model.max[0] = std::max<int>(model.max[0], x);
				model.min[1] = std::min<int>(model.min[1], y); model.max[1] = std::max<int>(model.max[1], y);
				model.min[2] = std::min<int>(model.min[2], 0); model.max[2] = std::max<int>(model.max[2], 0);
			}
		}
	}
	else
	{
		for (int along = 0; along < 16; ++along)
		{
			for (int z = 0; z < 24; ++z)
			{
				int x, y;
				planeVoxel(axis, offset, along, x, y);
				place(x, y, z);
			}
		}
	}
	model.ghostly = !model.empty;
}

/**
 * Builds a part the game gives no voxels from the shape the mod gives it (`firstPerson: shapes:`,
 * RuleFirstPerson::Shape; a player's report: the shops' metal racks can't be walked into, yet
 * they had no voxels to show in 3D): the voxels of its boxes, each coloured by the pixel it
 * lands on (the projection buildModel uses), and none where that pixel is transparent, so the
 * art's own outline trims the boxes (the legs stay legs, the gap under a shelf stays open, a box
 * drawn a little too big loses what the art doesn't show). The model comes out ghostly, as
 * buildGhost's: the game's shots and sight pass through the part.
 * @param model The empty model to build it in.
 * @param sprite The part's sprite frame.
 * @param shape Its boxes.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
void buildShape(FirstPersonView::VoxelModel &model, const Surface *sprite, const RuleFirstPerson::Shape &shape, int yOffset)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	for (const auto &box : shape.boxes)
	{
		for (int z = box[2]; z <= box[5]; ++z)
		{
			for (int y = box[1]; y <= box[4]; ++y)
			{
				for (int x = box[0]; x <= box[3]; ++x)
				{
					// The pixel the voxel lands on (see buildModel); none outside the sprite.
					const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
					const Uint8 color = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
					if (!color) continue;
					model.color[z][y][x] = color;
					model.empty = false;
					const int voxel[3] = { x, y, z };
					for (int axis = 0; axis < 3; ++axis)
					{
						model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
						model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
					}
				}
			}
		}
	}
	model.ghostly = !model.empty;
}

/**
 * Does a part without a shape stand low on its own tile (setting "Parts without shape" at Lifts,
 * asked for objects only: a floor without voxels is drawn as it was,
 * a player's reports, GitHub issues #56 and #52: X-Com Files' and XPiratez's crops, X-Com Files'
 * broken glass had no 3D shape and showed as nothing)? Its drawn pixels inside the floor's
 * diamond (as buildModel projects it: 32 pixels wide, 16 high, its top corner at pixel (16, 24))
 * swept up by LowArtMaxHeight pixels (a voxel a pixel up), all but LowArtOutside of them: a
 * crop, a heap, a scatter of glass; not a wall, a lamp post or anything reaching up or beyond.
 * @param sprite The part's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @param rises Set to whether some of it rises above the floor's own diamond (a crop, a heap);
 *   art lying wholly inside it may be paint on the floor (TFTD's Triton has such a patch by its
 *   nose), which buildRelief lays on the floor instead of standing it up.
 * @return True for low art (and some drawn at all).
 */
bool lowArt(const Surface *sprite, int yOffset, bool &rises)
{
	// Is a pixel inside the floor's diamond lifted by some pixels? On each of the diamond's 16 rows
	// its half width grows by two pixels a row down to its widest row, then shrinks again.
	auto inLiftedDiamond = [&](int spriteX, int spriteY, int lift)
	{
		const int row = spriteY + lift - 24 - yOffset;
		const int halfWidth = row < 0 || row > 15 ? -1 : row < 8 ? (row + 1) * 2 : (16 - row) * 2;
		return halfWidth > 0 && spriteX >= 16 - halfWidth && spriteX < 16 + halfWidth;
	};

	int inside = 0, outside = 0, rising = 0;
	for (int spriteY = 0; spriteY < sprite->getHeight(); ++spriteY)
	{
		for (int spriteX = 0; spriteX < sprite->getWidth(); ++spriteX)
		{
			if (!sprite->getPixel(spriteX, spriteY)) continue;
			bool low = false;
			for (int lift = 0; lift <= LowArtMaxHeight && !low; ++lift)
			{
				low = inLiftedDiamond(spriteX, spriteY, lift);
			}
			(low ? inside : outside) += 1;
			if (low && !inLiftedDiamond(spriteX, spriteY, 0)) ++rising;
		}
	}
	rises = rising > 0;
	return inside > 0 && outside <= (inside + outside) * LowArtOutside;
}

/**
 * Builds the ghost of low art (see lowArt: a crop, a heap, a scatter of glass) standing up as the
 * map draws it (the owner: the isometric map is the reference; laid flat, as buildGhost lays a
 * lift's pad, it sank into the floor under it and showed as nothing). Each column of the sprite
 * crosses the floor's diamond along one line of voxels, front to back (buildModel's projection).
 * Read from the bottom up: where a drawn pixel meets the diamond, the run of drawn pixels above it
 * (at most LowArtMaxHeight + 1) stands there as a column of voxels, each the colour of its pixel;
 * drawn pixels above that run, where they meet the diamond again, stand further back. From the
 * map's side it looks as the map draws it; from elsewhere, as thin cards. Art lying flat inside
 * the floor's diamond (paint on the floor, shards of glass) is laid on the floor instead: one
 * layer of voxels on top of a floor's slab (LowArtFlatLayer), each the colour of the pixel the
 * floor shows there. A ghost that the view draws solid, as the map does
 * (VoxelModel::solidGhost), not seen through on some pixels: in the game nothing stops at it,
 * but a field of crops is no lift pad to watch through.
 * @param model The empty model to build it in.
 * @param sprite The part's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @param rises Whether the art rises above the floor's diamond (see lowArt).
 * @param leftOut The pixels to leave out, as if transparent (width x height, a row after another:
 *   a block's own, see buildCropBlock); null for none.
 * @param baseZ The height the art stands on, in voxels: 0 for the floor; a block's top for what
 *   stands on it (rising art only).
 */
void buildRelief(FirstPersonView::VoxelModel &model, const Surface *sprite, int yOffset, bool rises, const std::vector<char> *leftOut = nullptr, int baseZ = 0)
{
	// The colour of a pixel; 0 (transparent) outside the sprite or left out.
	const int width = sprite->getWidth(), height = sprite->getHeight();
	auto pixelAt = [&](int spriteX, int spriteY) -> Uint8
	{
		if (spriteX < 0 || spriteY < 0 || spriteX >= width || spriteY >= height) return 0;
		if (leftOut && (*leftOut)[spriteY * width + spriteX]) return 0;
		return sprite->getPixel(spriteX, spriteY);
	};
	// A voxel in the model, its bounds grown round it.
	auto place = [&](int x, int y, int z, Uint8 color)
	{
		model.color[z][y][x] = color;
		model.empty = false;
		const int voxel[3] = { x, y, z };
		for (int boxAxis = 0; boxAxis < 3; ++boxAxis)
		{
			model.min[boxAxis] = std::min<int>(model.min[boxAxis], voxel[boxAxis]);
			model.max[boxAxis] = std::max<int>(model.max[boxAxis], voxel[boxAxis]);
		}
	};

	if (!rises)
	{
		// Flat: each voxel of the floor's diamond the colour of its pixel, on top of the slab.
		for (int y = 0; y < 16; ++y)
		{
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = pixelAt(16 + x - y, 24 + (x + y) / 2 + yOffset);
				if (color) place(x, y, LowArtFlatLayer, color);
			}
		}
		model.ghostly = model.solidGhost = !model.empty;
		return;
	}

	for (int spriteX = 0; spriteX < width; ++spriteX)
	{
		// The voxels this column crosses: those with x - y = spriteX - 16, one a row, the front
		// one (the highest x + y) lowest.
		const int diagonal = spriteX - 16;
		if (diagonal < -15 || diagonal > 15) continue;
		int claimedTop = INT_MAX; // the highest row a run nearer the front took
		for (int y = std::min(15, 15 - diagonal); y >= std::max(0, -diagonal); --y)
		{
			const int x = y + diagonal;
			const int floorRow = 24 + (x + y) / 2 - baseZ + yOffset;
			if (floorRow >= claimedTop || !pixelAt(spriteX, floorRow)) continue;

			// The run of drawn pixels up from here, a voxel a pixel.
			int run = 0;
			while (run <= LowArtMaxHeight && baseZ + run < Position::TileZ && pixelAt(spriteX, floorRow - run)) ++run;
			for (int z = 0; z < run; ++z)
			{
				place(x, y, baseZ + z, pixelAt(spriteX, floorRow - z));
			}
			claimedTop = floorRow - run + 1;
		}
	}
	model.ghostly = model.solidGhost = !model.empty;
}

/**
 * Tells whether a floor's art is a box of liquid, and how tall: XPiratez's sewers under its bases
 * are a green block about half a level tall (a player: "very intentionally about thigh deep"),
 * though the game gives the record only the floor's thin slab of voxels. The art's outline (its
 * drawn pixels) is compared with that of a tile-wide box of each height from LiquidMinHeight to
 * LiquidMaxHeight: the floor's diamond (where buildModel projects the floor: 32 pixels wide, 16
 * high, its top corner at pixel (16, 24)) swept up by that many pixels.
 * @param sprite The floor's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @return The box's height in voxels, where its outline and the art's share LiquidOutlineMatch
 *   of their pixels together; 0 for none.
 */
int boxHeightOf(const Surface *sprite, int yOffset, int minHeight, int maxHeight, float minMatch);
int liquidHeightOf(const Surface *sprite, int yOffset)
{
	return boxHeightOf(sprite, yOffset, LiquidMinHeight, LiquidMaxHeight, LiquidOutlineMatch);
}

/**
 * The height of the tile-wide box whose outline the art matches (see liquidHeightOf, cropBlockOf):
 * the art's outline (its drawn pixels) against that of a box of each height from minHeight to
 * maxHeight, the floor's diamond swept up by that many pixels.
 * @param sprite The part's sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @param minHeight, maxHeight The heights to try, in voxels.
 * @param minMatch The share of the two outlines' pixels together they must share at least.
 * @return The best matching box's height in voxels; 0 for none matching well enough.
 */
int boxHeightOf(const Surface *sprite, int yOffset, int minHeight, int maxHeight, float minMatch)
{
	// Is a pixel inside the floor's diamond lifted by some pixels? On each of the diamond's 16 rows
	// its half width grows by two pixels a row down to its widest row, then shrinks again.
	auto inLiftedDiamond = [&](int spriteX, int spriteY, int lift)
	{
		const int row = spriteY + lift - 24 - yOffset;
		if (row < 0 || row > 15) return false;
		const int halfWidth = row < 8 ? (row + 1) * 2 : (16 - row) * 2;
		return spriteX >= 16 - halfWidth && spriteX < 16 + halfWidth;
	};
	const int width = sprite->getWidth(), height = sprite->getHeight();
	int bestHeight = 0;
	float bestMatch = 0.0f;
	for (int boxHeight = minHeight; boxHeight <= maxHeight; ++boxHeight)
	{
		// The box's outline: the diamond swept up from the floor to the box's top.
		int both = 0, either = 0;
		for (int spriteY = 0; spriteY < height; ++spriteY)
		{
			for (int spriteX = 0; spriteX < width; ++spriteX)
			{
				bool inBox = false;
				for (int lift = 0; lift <= boxHeight && !inBox; ++lift) inBox = inLiftedDiamond(spriteX, spriteY, lift);
				const bool drawn = sprite->getPixel(spriteX, spriteY) != 0;
				both += inBox && drawn;
				either += inBox || drawn;
			}
		}
		const float match = either ? (float)both / either : 0.0f;
		if (match > bestMatch)
		{
			bestMatch = match;
			bestHeight = boxHeight;
		}
	}
	return bestMatch >= minMatch ? bestHeight : 0;
}

/**
 * Builds the surface of a floor the art draws as a box of liquid (see liquidHeightOf): a ghost
 * (see VoxelModel::ghostly), one layer of voxels across the tile at the box's height, each
 * coloured by the pixel of the box's top it lands on (the projection buildModel uses). Soldiers
 * wading through it show through it on some pixels from their thighs down, as the map draws them
 * sunk in it; the game's shots and sight pass through it, as the game gives it no voxels. With
 * the setting "Parts without shape" off, as no ghost shows, there is none (the model stays empty).
 * @param model The empty model to build it in.
 * @param sprite The floor's sprite frame.
 * @param surfaceHeight The box's height in voxels (VoxelModel::liquidHeight).
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
void buildLiquid(FirstPersonView::VoxelModel &model, const Surface *sprite, int surfaceHeight, int yOffset)
{
	if (Options::fpsGhostParts == 0)
	{
		return;
	}
	const int layer = std::min(surfaceHeight, Position::TileZ - 1), width = sprite->getWidth(), height = sprite->getHeight();
	for (int y = 0; y < Position::TileXY; ++y)
	{
		for (int x = 0; x < Position::TileXY; ++x)
		{
			const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - layer + yOffset;
			const Uint8 color = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
			if (!color) continue;
			model.color[layer][y][x] = color;
			model.empty = false;
			const int voxel[3] = { x, y, layer };
			for (int axis = 0; axis < 3; ++axis)
			{
				model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
				model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
			}
		}
	}
	model.ghostly = !model.empty;
	if (!model.empty) colorHiddenSides(model, sprite);
}

/**
 * Tells whether an object's art is a block of crops, and how tall (a player, GitHub issue #56: X-Com
 * Files' wheat fields "have no wheat"): its art a tile-wide block CropBlockMinHeight to
 * CropBlockMaxHeight voxels tall (see boxHeightOf, CropBlockMatch), while its own voxels fill under
 * CropBlockVoxelShare of the block (the wheat: the floor's slab and four thin posts up to half its
 * height), so the 3D view showed thin posts where the map draws a field. Only what one walks through
 * as through a field: walkable, at the floor's own level (not a mound one stands on, as TFTD's
 * islands have; nor a UFO's hull or a colony's wall, drawn as tall blocks no one walks through).
 * None with "Parts without shape" off, and none for a door.
 * @param model The object's model built from its voxels; null for an object without voxels.
 * @param mapData The object's record.
 * @param sprite Its sprite frame.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @return The block's height in voxels; 0 for none.
 */
int cropBlockOf(const FirstPersonView::VoxelModel *model, const MapData *mapData, const Surface *sprite, int yOffset)
{
	if (Options::fpsGhostParts == 0 || mapData->getObjectType() != O_OBJECT || mapData->isDoor() || mapData->isUFODoor() || mapData->getAltMCD() != 0
		|| mapData->getTUCost(MT_WALK) >= Pathfinding::INVALID_MOVE_COST || mapData->getTerrainLevel() != 0)
	{
		return 0;
	}
	const int blockHeight = boxHeightOf(sprite, yOffset, CropBlockMinHeight, CropBlockMaxHeight, CropBlockMatch);
	if (!blockHeight || !model)
	{
		return blockHeight;
	}

	// The object's own voxels inside the block, against the block's.
	const int top = std::min(blockHeight, Position::TileZ - 1);
	int inside = 0;
	for (int z = 0; z <= top; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
				inside += model->color[z][y][x] != 0;
	return inside < Position::TileXY * Position::TileXY * (top + 1) * CropBlockVoxelShare ? blockHeight : 0;
}

/**
 * Builds a block of crops (see cropBlockOf; the owner: a solid block, as the map draws the field): a
 * ghost the view draws solid (VoxelModel::solidGhost), the block's shell (its top layer at the
 * block's height and its four sides down to the floor), each voxel the colour of the pixel it lands
 * on (the projection buildModel uses; where the art has a hole, its commonest colour). In a field
 * the tiles' sides inside it are hidden by their neighbours', so the field shows sides only where
 * it ends; whoever stands in it is hidden below its top, and a soldier kneeling in it sees the
 * crops round him (the shell's inside). What the art draws above the block (the wheat's ears)
 * stands on its top (see buildRelief). The game's shots, sight and walking go through it, as the
 * game gives it no such voxels.
 * @param model The empty model to build it in.
 * @param sprite The object's sprite frame.
 * @param blockHeight The block's height in voxels (VoxelModel::cropBlock).
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
void buildCropBlock(FirstPersonView::VoxelModel &model, const Surface *sprite, int blockHeight, int yOffset)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	const int top = std::min(blockHeight, Position::TileZ - 1);

	// The art's commonest colour, for its holes.
	int counts[256] = {};
	for (int spriteY = 0; spriteY < height; ++spriteY)
		for (int spriteX = 0; spriteX < width; ++spriteX) ++counts[sprite->getPixel(spriteX, spriteY)];
	counts[0] = 0;
	const Uint8 common = (Uint8)(std::max_element(counts, counts + 256) - counts);

	// The shell: the top layer and the four sides.
	for (int z = 0; z <= top; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (z != top && x != 0 && y != 0 && x != Position::TileXY - 1 && y != Position::TileXY - 1) continue;
				const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
				const Uint8 color = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
				model.color[z][y][x] = color ? color : common;
			}
	model.empty = false;
	model.min[0] = model.min[1] = model.min[2] = 0;
	model.max[0] = model.max[1] = Position::TileXY - 1;
	model.max[2] = top;

	// What the art draws above the block's outline (the floor's diamond swept up to its top): standing
	// on its top.
	std::vector<char> blockPixels(width * height, 0);
	for (int spriteY = 0; spriteY < height; ++spriteY)
		for (int spriteX = 0; spriteX < width; ++spriteX)
			for (int lift = 0; lift <= blockHeight && !blockPixels[spriteY * width + spriteX]; ++lift)
			{
				const int row = spriteY + lift - 24 - yOffset;
				const int halfWidth = row < 0 || row > 15 ? -1 : row < 8 ? (row + 1) * 2 : (16 - row) * 2;
				blockPixels[spriteY * width + spriteX] = halfWidth > 0 && spriteX >= 16 - halfWidth && spriteX < 16 + halfWidth;
			}
	if (top + 1 < Position::TileZ)
	{
		buildRelief(model, sprite, yOffset, true, &blockPixels, top + 1);
	}
	model.ghostly = model.solidGhost = true;
	colorHiddenSides(model, sprite);
}

/**
 * Tells whether a part's art and shape are a mass of foliage (a player, GitHub issue #59: X-Com
 * Files' forest "in iso they look good, in first, they look like cubes of playdoh": its big pines'
 * crowns are solid boxes, three tiles by three and two levels tall, which the map shows only from
 * above and the south-east): its drawn pixels mostly green (FoliageGreenShare, a pixel greener than
 * it is red or blue), and its voxels filling a good part of the tile (FoliageBulkShare). Whether it
 * is drawn leafy is the tile's to say (see leafyModel: only on a tile without a floor, a crown, not
 * a green wall of an alien base standing on one).
 * @param model The part's model, built from its voxels.
 * @param sprite Its sprite frame.
 * @param palette The palette its colours are in.
 * @return True for foliage.
 */
bool foliageArt(const FirstPersonView::VoxelModel &model, const Surface *sprite, const SDL_Color *palette)
{
	if (model.empty)
	{
		return false;
	}
	int drawn = 0, green = 0, voxels = 0;
	for (int spriteY = 0; spriteY < sprite->getHeight(); ++spriteY)
		for (int spriteX = 0; spriteX < sprite->getWidth(); ++spriteX)
		{
			const Uint8 color = sprite->getPixel(spriteX, spriteY);
			if (!color) continue;
			++drawn;
			const SDL_Color &rgb = palette[color];
			green += rgb.g > rgb.r + FoliageGreenMargin && rgb.g > rgb.b + FoliageGreenMargin;
		}
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x) voxels += model.color[z][y][x] != 0;
	return drawn > 0 && green >= drawn * FoliageGreenShare && voxels >= Position::TileXY * Position::TileXY * Position::TileZ * FoliageBulkShare;
}

/**
 * Makes a model of foliage leafy (see foliageArt, leafyModel; the owner: holes in it, more towards
 * its edges, so sky and light show through, and it reads as leaves, not as a box): voxels left out
 * by a fixed pattern (a hash of the voxel and the part's sprite, the same every time), the more of
 * them the nearer the surface: LeafHoleChance by the voxel's depth (1 for a voxel touching the
 * outside or the tile's side, then inwards), none deeper. Only a picture: the game's shots and
 * sight go by its own voxels.
 * @param model The model, built from its voxels.
 * @param seed The pattern's seed (the part's sprite).
 */
void carveLeaves(FirstPersonView::VoxelModel &model, unsigned seed)
{
	// Each voxel's depth: 0 for air, 1 for one touching air or the tile's side, then inwards.
	const int deepest = (int)(sizeof(LeafHoleChance) / sizeof(LeafHoleChance[0]));
	int depth[Position::TileZ][Position::TileXY][Position::TileXY];
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x) depth[z][y][x] = model.color[z][y][x] ? deepest + 1 : 0;
	const int steps[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int pass = 0; pass < deepest; ++pass)
		for (int z = 0; z < Position::TileZ; ++z)
			for (int y = 0; y < Position::TileXY; ++y)
				for (int x = 0; x < Position::TileXY; ++x)
				{
					if (!depth[z][y][x]) continue;
					for (const auto &step : steps)
					{
						const int nx = x + step[0], ny = y + step[1], nz = z + step[2];
						const bool outside = nx < 0 || ny < 0 || nz < 0 || nx >= Position::TileXY || ny >= Position::TileXY || nz >= Position::TileZ;
						depth[z][y][x] = std::min(depth[z][y][x], (outside ? 0 : depth[nz][ny][nx]) + 1);
					}
				}

	// The holes.
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
			{
				const int voxelDepth = depth[z][y][x];
				if (!voxelDepth || voxelDepth > deepest) continue;
				unsigned hash = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u ^ seed;
				hash ^= hash >> 13;
				hash *= 0x5bd1e995u;
				hash ^= hash >> 15;
				if ((int)(hash % 100) < LeafHoleChance[voxelDepth - 1]) model.color[z][y][x] = 0;
			}
}


/**
 * Tells whether a floor's or a wall's voxels leave out most of its art (a player's report: XPiratez's
 * catwalks in Chem Town "look like nothing", their gratings given nine voxels a layer and their
 * railings posts on every other layer, so shots go through, while the art draws a whole grating, a
 * rail, a chain-link mesh): its voxels all lie in its own plane (a floor's, ArtFillPlaneDepth voxels
 * up from its bottom; a wall's, as deep in from its edge) and fill under ArtFillShare of the cells of
 * that plane the art draws (a floor's columns whose top lands on a drawn pixel, a wall's cells of its
 * face; ArtFillMinCells of them at least). A part whose voxels stand elsewhere in the tile is left as
 * it is: the rest of its art would land at the wrong depth.
 * @param model The part's model, built from its voxels.
 * @param sprite Its sprite frame.
 * @param partType O_FLOOR, O_WESTWALL or O_NORTHWALL (anything else: no).
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @return True if the rest of its art should be drawn as a ghost (see buildArtFill).
 */
bool sparseArt(const FirstPersonView::VoxelModel &model, const Surface *sprite, int partType, int yOffset)
{
	if (model.empty)
	{
		return false;
	}
	// Does the art draw the pixel a voxel lands on (the projection buildModel uses)?
	const int width = sprite->getWidth(), height = sprite->getHeight();
	auto drawn = [&](int x, int y, int z)
	{
		const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
		return spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height && sprite->getPixel(spriteX, spriteY) != 0;
	};
	int artCells = 0, filledCells = 0;
	if (partType == O_FLOOR && model.max[2] < ArtFillPlaneDepth)
	{
		// A floor: its columns, the art at its top.
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (!drawn(x, y, model.max[2])) continue;
				++artCells;
				bool filled = false;
				for (int z = 0; z <= model.max[2] && !filled; ++z) filled = model.color[z][y][x] != 0;
				filledCells += filled;
			}
	}
	else if ((partType == O_WESTWALL && model.max[0] < ArtFillPlaneDepth) || (partType == O_NORTHWALL && model.max[1] < ArtFillPlaneDepth))
	{
		// A wall: the cells of its face (along it and up), the art on its outer side.
		const bool west = partType == O_WESTWALL;
		for (int z = 0; z < Position::TileZ; ++z)
			for (int along = 0; along < Position::TileXY; ++along)
			{
				if (!drawn(west ? 0 : along, west ? along : 0, z)) continue;
				++artCells;
				bool filled = false;
				for (int in = 0; in < ArtFillPlaneDepth && !filled; ++in) filled = (west ? model.color[z][along][in] : model.color[z][in][along]) != 0;
				filledCells += filled;
			}
	}
	return artCells >= ArtFillMinCells && filledCells < artCells * ArtFillShare;
}

/**
 * Builds the rest of the art of a floor or wall whose voxels leave most of it out (see sparseArt): a
 * ghost (see VoxelModel::ghostly) in the part's own plane, a voxel wherever the art draws a pixel and
 * the part has no voxel in that column or cell (on a floor's top layer; on a wall's outer layer, at its
 * edge), coloured by that pixel, so a grating keeps its holes and a fence its mesh where the art has
 * them. The part's own voxels stay as they are, solid, as the game has them. With the setting "Parts
 * without shape" off, as no ghost shows, there is none (the model stays empty).
 * @param fill The empty model to build it in.
 * @param part The part's own model.
 * @param sprite Its sprite frame.
 * @param partType O_FLOOR, O_WESTWALL or O_NORTHWALL.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 */
void buildArtFill(FirstPersonView::VoxelModel &fill, const FirstPersonView::VoxelModel &part, const Surface *sprite, int partType, int yOffset)
{
	if (Options::fpsGhostParts == 0)
	{
		return;
	}
	const int width = sprite->getWidth(), height = sprite->getHeight();
	// A voxel of the fill where the art draws the pixel it lands on, in that pixel's colour.
	auto put = [&](int x, int y, int z)
	{
		const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
		const Uint8 color = spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
		if (!color) return;
		fill.color[z][y][x] = color;
		fill.empty = false;
		const int voxel[3] = { x, y, z };
		for (int axis = 0; axis < 3; ++axis)
		{
			fill.min[axis] = std::min<int>(fill.min[axis], voxel[axis]);
			fill.max[axis] = std::max<int>(fill.max[axis], voxel[axis]);
		}
	};
	if (partType == O_FLOOR)
	{
		const int top = part.max[2];
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
			{
				bool filled = false;
				for (int z = 0; z <= top && !filled; ++z) filled = part.color[z][y][x] != 0;
				if (!filled) put(x, y, top);
			}
	}
	else
	{
		const bool west = partType == O_WESTWALL;
		for (int z = 0; z < Position::TileZ; ++z)
			for (int along = 0; along < Position::TileXY; ++along)
			{
				bool filled = false;
				for (int in = 0; in < ArtFillPlaneDepth && !filled; ++in) filled = (west ? part.color[z][along][in] : part.color[z][in][along]) != 0;
				if (!filled) put(west ? 0 : along, west ? along : 0, z);
			}
	}
	fill.ghostly = !fill.empty;
	if (!fill.empty) colorHiddenSides(fill, sprite);
}

}

/**
 * Finds the crafts and UFOs on the map, so their hidden sides can be mirrored (mirroredColor).
 *
 * A craft is a group of connected tiles built from the terrains of crafts and UFOs (and of no
 * other terrain). The isometric art shows only their east and south sides; their west and
 * north sides show whatever the projection puts there (the Skyranger's west flank is a thin
 * shell drawn only from the inside, its east one a solid block). Crafts are symmetric, so a
 * hidden side can borrow the colours of the side opposite. For each craft this finds the box
 * of its voxels, whether its shape mirrors east-west and north-south, and for each row of
 * voxels the open east / south faces (see the Mirror struct). Runs once per battle; coordinates
 * are world voxels (16 per tile across, 24 up).
 */
void FirstPersonView::findMirrors()
{
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ(),
		tileCount = mapSizeX * mapSizeY * mapSizeZ;
	_mirrors.clear();
	_mirrorOfTile.assign(tileCount, -1);
	const Mod *mod = _save->getMod();
	if (!mod)
	{
		return;
	}
	// A view that borrows another's models (the soldier's card, the cameras: see shareCache) takes
	// its map of the crafts too, once that view has made it for this battle: the map is the same
	// for every view, and making it took some 20 ms each, four or five times at a battle's start.
	const FirstPersonView *sharedWith = _sharedModelsOwner;
	if (sharedWith && sharedWith->_save == _save && (int)sharedWith->_mirrorOfTile.size() == tileCount)
	{
		_mirrors = sharedWith->_mirrors;
		_mirrorOfTile = sharedWith->_mirrorOfTile;
		return;
	}
	const Uint32 startTick = SDL_GetTicks();

	// The names of the map data sets that crafts and UFOs are built from, minus any that an
	// ordinary terrain uses too.
	std::set<std::string> craftSets, terrainSets, ownCraftSets; // (ownCraftSets: the player's crafts' alone)
	auto collect = [](RuleTerrain *terrain, std::set<std::string> &into)
	{
		if (!terrain) return;
		for (const MapDataSet *dataSet : *terrain->getMapDataSets()) into.insert(dataSet->getName());
	};
	for (const auto &type : mod->getCraftsList()) collect(mod->getCraft(type)->getBattlescapeTerrainData(), craftSets);
	// The player's own craft: the one landed in this battle (a mod may build other things from a
	// craft's map data, XPiratez its temples); without one to ask, every craft the mod has.
	// And whether a UFO is in this battle at all: on a mission site there is none, whatever its
	// buildings are made of (lifts and all).
	bool ufoInBattle = true;
	if (SavedGame *game = _game->getSavedGame())
	{
		for (Base *base : *game->getBases())
			for (Craft *craft : *base->getCrafts())
				if (craft->isInBattlescape()) collect(craft->getRules()->getBattlescapeTerrainData(), ownCraftSets);
		ufoInBattle = false;
		for (Ufo *ufo : *game->getUfos()) ufoInBattle = ufoInBattle || ufo->isInBattlescape();
	}
	else
	{
		for (const auto &type : mod->getCraftsList()) collect(mod->getCraft(type)->getBattlescapeTerrainData(), ownCraftSets);
	}
	for (const auto &type : mod->getUfosList()) collect(mod->getUfo(type)->getBattlescapeTerrainData(), craftSets);
	for (const auto &type : mod->getTerrainList()) collect(mod->getTerrain(type), terrainSets);
	for (const auto &setName : terrainSets) craftSets.erase(setName); // (BLANKS and the like are everywhere)
	for (const auto &setName : terrainSets) ownCraftSets.erase(setName);
	if (craftSets.empty())
	{
		return;
	}

	// Each tile's parts as first drawn (null for none, an empty model or a ghost, which is never
	// solid); a tile is part of a craft if any of its parts comes from a craft's data set.
	std::vector<std::array<const VoxelModel*, 4>> tileModels(tileCount);
	std::vector<char> isCraftTile(tileCount, 0);
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		const Tile *tile = _save->getTile(tileIndex);
		for (int part = 0; part < 4; ++part)
		{
			const MapData *mapData = tile->getMapData((TilePart)part);
			const VoxelModel *model = mapData ? getModel(mapData, -1, false) : nullptr;
			tileModels[tileIndex][part] = model && !model->empty && !model->ghostly ? model : nullptr;
			if (tileModels[tileIndex][part] && mapData->getDataset() && craftSets.count(mapData->getDataset()->getName()))
				isCraftTile[tileIndex] = 1;
		}
	}

	// Is there something solid at a world voxel? Anything at all (owner < 0), or only the tiles
	// of the given craft (owner = its index in _mirrors). Outside the map nothing is.
	auto solidAt = [&](int worldX, int worldY, int worldZ, int owner)
	{
		if (worldX < 0 || worldY < 0 || worldZ < 0 || worldX >= mapSizeX * 16 || worldY >= mapSizeY * 16 || worldZ >= mapSizeZ * 24)
			return false;
		const int tileIndex = ((worldZ / 24) * mapSizeY + worldY / 16) * mapSizeX + worldX / 16;
		if (owner >= 0 && _mirrorOfTile[tileIndex] != owner) return false;
		for (const VoxelModel *model : tileModels[tileIndex])
		{
			if (model && model->color[worldZ % 24][worldY % 16][worldX % 16]) return true;
		}
		return false;
	};

	// Every craft tile not yet given to a craft starts a new craft (at most 64 of them).
	std::vector<int> craftTiles, toVisit;
	for (int seed = 0; seed < tileCount; ++seed)
	{
		if (!isCraftTile[seed] || _mirrorOfTile[seed] >= 0 || _mirrors.size() >= 64) continue;
		const int craftId = (int)_mirrors.size();
		_mirrors.emplace_back();
		Mirror &mirror = _mirrors.back();

		// Flood fill from the seed: gather the craft's connected tiles (touching in any of the
		// 26 directions, diagonals and levels included) and the box of their solid voxels.
		craftTiles.clear();
		toVisit.assign(1, seed);
		_mirrorOfTile[seed] = craftId;
		int voxelLow[3] = { INT_MAX, INT_MAX, INT_MAX }, voxelHigh[3] = { INT_MIN, INT_MIN, INT_MIN };
		while (!toVisit.empty())
		{
			const int tileIndex = toVisit.back();
			toVisit.pop_back();
			craftTiles.push_back(tileIndex);
			const int tilePos[3] = { tileIndex % mapSizeX, (tileIndex / mapSizeX) % mapSizeY, tileIndex / (mapSizeX * mapSizeY) };

			// Grow the box by the solid voxels of this tile's parts.
			for (const VoxelModel *model : tileModels[tileIndex])
			{
				if (!model) continue;
				for (int axis = 0; axis < 3; ++axis)
				{
					const int voxelsPerTile = axis < 2 ? 16 : 24;
					voxelLow[axis] = std::min(voxelLow[axis], tilePos[axis] * voxelsPerTile + model->min[axis]);
					voxelHigh[axis] = std::max(voxelHigh[axis], tilePos[axis] * voxelsPerTile + model->max[axis]);
				}
			}

			// Queue the neighbouring craft tiles that belong to no craft yet.
			for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int neighbourX = tilePos[0] + dx, neighbourY = tilePos[1] + dy, neighbourZ = tilePos[2] + dz;
						if (neighbourX < 0 || neighbourY < 0 || neighbourZ < 0
							|| neighbourX >= mapSizeX || neighbourY >= mapSizeY || neighbourZ >= mapSizeZ) continue;
						const int neighbour = (neighbourZ * mapSizeY + neighbourY) * mapSizeX + neighbourX;
						if (isCraftTile[neighbour] && _mirrorOfTile[neighbour] < 0)
						{
							_mirrorOfTile[neighbour] = craftId;
							toVisit.push_back(neighbour);
						}
					}
		}
		// A building the mod's rules list among its UFOs (XPiratez lands its enemies in temples,
		// mansions and bunkers that way) is no craft to mirror: a building isn't symmetric as a hull
		// is, and its walls came out in patches, the far side's look where the mirror found any and
		// their own elsewhere (a player, GitHub issue #156: a tower room's stone wall wore the planks
		// opposite in the outline of the stairs there). Told as the UFOs' lights tell it (see
		// findUfos): a UFO has a power source, a navigation console or a grav lift, and there is one in
		// the battle (a temple has its lifts too). The player's own craft, the one landed here, has
		// none and is mirrored as ever. A building's walls take their paint by its
		// rooms instead (see findWallPaints).
		bool machinery = false, ownCraft = false;
		for (const int tileIndex : craftTiles)
		{
			const Tile *tile = _save->getTile(tileIndex);
			for (int part = 0; part < 4; ++part)
			{
				const MapData *mapData = tile->getMapData((TilePart)part);
				if (!mapData) continue;
				const SpecialTileType special = mapData->getSpecialType();
				machinery = machinery || special == UFO_POWER_SOURCE || special == UFO_NAVIGATION || (part == O_FLOOR && mapData->isGravLift());
				ownCraft = ownCraft || (mapData->getDataset() && ownCraftSets.count(mapData->getDataset()->getName()));
			}
		}
		if (!ownCraft && !(machinery && ufoInBattle))
		{
			for (const int tileIndex : craftTiles)
			{
				_mirrorOfTile[tileIndex] = -1;
				isCraftTile[tileIndex] = 0;
			}
			_mirrors.pop_back();
			Log(LOG_INFO) << "OpenXcom3D: a building of " << craftTiles.size() << " tiles from a craft's or a UFO's map (" << (ufoInBattle ? "no power source, navigation or grav lift" : "no UFO in this battle") << "): not mirrored as a craft";
			continue;
		}
		// The craft's box: its first voxel and its size along each axis.
		for (int axis = 0; axis < 3; ++axis)
		{
			mirror.lo[axis] = voxelLow[axis];
			mirror.size[axis] = voxelHigh[axis] - voxelLow[axis] + 1;
		}

		// Mirror east-west (flipping x), then north-south (flipping y).
		for (int axis = 0; axis < 2; ++axis)
		{
			// The mirror line runs through the middle of the box; keeping it doubled (low +
			// high) keeps it a whole number: a voxel at w mirrors to sum - w.
			mirror.sum[axis] = voxelLow[axis] + voxelHigh[axis];

			// Does it mirror? Yes if at least 70% of its solid voxels have a solid twin across
			// the middle.
			int solidVoxels = 0, twinVoxels = 0;
			for (const int tileIndex : craftTiles)
			{
				const int tilePos[3] = { tileIndex % mapSizeX, (tileIndex / mapSizeX) % mapSizeY, tileIndex / (mapSizeX * mapSizeY) };
				for (int voxelZ = 0; voxelZ < 24; ++voxelZ)
					for (int voxelY = 0; voxelY < 16; ++voxelY)
						for (int voxelX = 0; voxelX < 16; ++voxelX)
						{
							int worldVoxel[3] = { tilePos[0] * 16 + voxelX, tilePos[1] * 16 + voxelY, tilePos[2] * 24 + voxelZ };
							if (!solidAt(worldVoxel[0], worldVoxel[1], worldVoxel[2], craftId)) continue;
							++solidVoxels;
							worldVoxel[axis] = mirror.sum[axis] - worldVoxel[axis];
							twinVoxels += solidAt(worldVoxel[0], worldVoxel[1], worldVoxel[2], craftId);
						}
			}
			mirror.symmetric[axis] = solidVoxels > 0 && twinVoxels * 10 >= solidVoxels * 7;

			// Index the open east (south) faces along every row of voxels that runs along the
			// axis: rows are numbered by height, then position across (row = up * acrossSize +
			// across). start[row] .. start[row + 1] is the row's slice of pos (a craft that
			// doesn't mirror this way keeps every row empty).
			const int alongSize = mirror.size[axis], acrossSize = mirror.size[1 - axis], rowCount = acrossSize * mirror.size[2];
			mirror.start[axis].assign(rowCount + 1, 0);
			mirror.pos[axis].clear();
			mirror.outer[axis].assign(rowCount, -1);
			if (!mirror.symmetric[axis]) continue;
			for (int row = 0; row < rowCount; ++row)
			{
				mirror.start[axis][row] = (int)mirror.pos[axis].size();
				int worldVoxel[3];
				worldVoxel[2] = voxelLow[2] + row / acrossSize;
				worldVoxel[1 - axis] = voxelLow[1 - axis] + row % acrossSize;
				// Walk the row from west to east (north to south).
				for (int offset = 0; offset < alongSize; ++offset)
				{
					worldVoxel[axis] = voxelLow[axis] + offset;
					if (!solidAt(worldVoxel[0], worldVoxel[1], worldVoxel[2], craftId)) continue;
					// The first solid voxel is the craft's outer skin on the west (north).
					if (mirror.outer[axis][row] < 0) mirror.outer[axis][row] = (Sint16)worldVoxel[axis];
					// A solid voxel with nothing at all east (south) of it has an open face there.
					int nextVoxel[3] = { worldVoxel[0], worldVoxel[1], worldVoxel[2] };
					++nextVoxel[axis];
					if (!solidAt(nextVoxel[0], nextVoxel[1], nextVoxel[2], -1)) mirror.pos[axis].push_back((Sint16)worldVoxel[axis]);
				}
			}
			mirror.start[axis][rowCount] = (int)mirror.pos[axis].size();
		}
		Log(LOG_INFO) << "OpenXcom3D: craft/UFO of " << craftTiles.size() << " tiles, mirrors east-west: " << (mirror.symmetric[0] ? "yes" : "no")
			<< ", north-south: " << (mirror.symmetric[1] ? "yes" : "no");
	}
	if (!_mirrors.empty())
	{
		Log(LOG_INFO) << "OpenXcom3D: crafts/UFOs mapped in " << SDL_GetTicks() - startTick << " ms";
	}
}

/**
 * Gets the colour of whatever is solid at a world voxel in this frame's tile snapshot (so what
 * has been blown away since the battle began is gone).
 * @param worldX Voxel position east, from the map's west edge (16 per tile).
 * @param worldY Voxel position south, from the map's north edge (16 per tile).
 * @param worldZ Voxel height, from the bottom of the map (24 per level).
 * @param nextFrame Read an animated part's next frame instead (a pixel already dissolved into it).
 * @param onlyPart Only this tile part counts (0 floor, 1 west wall, 2 north wall, 3 object); -1 any.
 * @param noWaysThrough A wall one goes through (a door, a curtain: VoxelModel::wayThrough) doesn't count.
 * @return The palette colour, 0 if nothing is there or the voxel is outside the map.
 */
Uint8 FirstPersonView::voxelColorAt(int worldX, int worldY, int worldZ, bool nextFrame, int onlyPart, bool noWaysThrough) const
{
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	if (worldX < 0 || worldY < 0 || worldZ < 0 || worldX >= mapSizeX * 16 || worldY >= mapSizeY * 16 || worldZ >= mapSizeZ * 24) return 0;
	// The first of the tile's parts that is solid at that voxel gives the colour (from the
	// part's next frame if asked and it has one).
	const TileView &tileView = _tileViews[((worldZ / 24) * mapSizeY + worldY / 16) * mapSizeX + worldX / 16];
	for (int part = 0; part < 4; ++part)
	{
		if (onlyPart >= 0 && part != onlyPart) continue;
		const VoxelModel *model = nextFrame && tileView.next[part] ? tileView.next[part] : tileView.part[part];
		if (model && noWaysThrough && model->wayThrough) continue;
		const Uint8 color = model ? model->color[worldZ % 24][worldY % 16][worldX % 16] : 0;
		if (color) return color;
	}
	return 0;
}

/**
 * Gets the colour a craft's hidden side should show, mirrored from the side the art does show.
 * For a face looking west (axis 0) or north (axis 1) it is the colour of the surface across the
 * craft's middle on the same row of voxels: for the craft's outer skin, the outer skin opposite;
 * for anything inside, the open east / south face nearest the mirrored spot (walls differ in
 * thickness, so the exact mirrored voxel may be inside a wall or in the air). A row with
 * nothing there borrows from the rows next to it. See findMirrors for the data it reads.
 * Two things are never borrowed: black (the art outlines a hull's edge in black, which made
 * black stripes on the inside of a UFO's stepped walls), and, inside the craft, the colour of
 * another kind of tile part (a craft's insides aren't quite symmetric: a wall took the
 * texture of the gore tubes standing opposite it in the Harvester). Then the face shows its
 * own row's colour instead (see shadeHit).
 * @param hit Where a ray hit a tile part.
 * @param axis 0 to mirror east-west, 1 to mirror north-south.
 * @param skinOnly Only answer for the craft's outer skin (0 for anything inside).
 * @param nextFrame Borrow from animated parts' next frames (a pixel already dissolved into them).
 * @return The mirrored colour; 0 if the hit is not on a craft, the craft doesn't mirror that
 *         way, or no row nearby has anything to borrow.
 */
Uint8 FirstPersonView::mirroredColor(const Hit &hit, int axis, bool skinOnly, bool nextFrame) const
{
	// Which craft is the hit tile part of, and does that craft mirror along this axis?
	const int craftId = _mirrorOfTile.empty() ? -1 : _mirrorOfTile[hit.tile];
	if (craftId < 0 || !_mirrors[craftId].symmetric[axis])
	{
		return 0;
	}
	// A wall one goes through (a door, a curtain) keeps its own look on both its faces: it says
	// "this way" from either side (a player, GitHub issue #156: a curtain was a curtain on one side
	// only, the other the wall's opposite).
	if (hit.part >= 0)
	{
		const VoxelModel *own = _tileViews[hit.tile].part[hit.part];
		if (own && own->wayThrough) return 0;
	}
	const Mirror &mirror = _mirrors[craftId];

	// The hit voxel in world voxels, and its row: position across the axis and height, both
	// counted from the craft's box corner.
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY();
	int worldVoxel[3] = {
		(hit.tile % mapSizeX) * 16 + hit.voxel[0],
		((hit.tile / mapSizeX) % mapSizeY) * 16 + hit.voxel[1],
		(hit.tile / (mapSizeX * mapSizeY)) * 24 + hit.voxel[2] };
	const int acrossPos = worldVoxel[1 - axis] - mirror.lo[1 - axis], upPos = worldVoxel[2] - mirror.lo[2],
		acrossSize = mirror.size[1 - axis];
	if (acrossPos < 0 || acrossPos >= acrossSize || upPos < 0 || upPos >= mirror.size[2])
	{
		return 0;
	}

	// Is the hit voxel the craft's outer skin (the first voxel of its row from the west / north)?
	const bool onSkin = mirror.outer[axis][upPos * acrossSize + acrossPos] == worldVoxel[axis];
	if (skinOnly && !onSkin)
	{
		return 0;
	}

	// Where the hit voxel lands mirrored across the craft's middle, along the axis.
	const int mirroredPos = mirror.sum[axis] - worldVoxel[axis];

	// Black, to the eye (darker than 14 of 255), in the view's palette.
	const SDL_Color *palette = getPalette();
	auto isBlack = [palette](Uint8 color) { return palette && palette[color].r * 299 + palette[color].g * 587 + palette[color].b * 114 < 14000; };

	// Look on the hit voxel's own row first, then on the rows beside it (across, then up and
	// down, then further across): offsets as { across, up }.
	static const int NearbyRows[][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -2, 0 }, { 2, 0 }, { -3, 0 }, { 3, 0 } };
	for (const auto &rowOffset : NearbyRows)
	{
		const int rowAcross = acrossPos + rowOffset[0], rowUp = upPos + rowOffset[1];
		if (rowAcross < 0 || rowAcross >= acrossSize || rowUp < 0 || rowUp >= mirror.size[2]) continue;

		// The row's open east / south faces, positions along the axis in increasing order.
		const int row = rowUp * acrossSize + rowAcross;
		const Sint16 *rowFirst = mirror.pos[axis].data() + mirror.start[axis][row],
			*rowEnd = mirror.pos[axis].data() + mirror.start[axis][row + 1];
		if (rowFirst == rowEnd) continue;

		// Pick the face to borrow from, no further than a tile from the mirrored spot.
		int bestPos = -1;
		if (onSkin)
		{
			// The outer skin borrows from the outer skin opposite: the row's last open face.
			if (std::abs(rowEnd[-1] - mirroredPos) <= 16) bestPos = rowEnd[-1];
		}
		else
		{
			// Anything inside borrows from the open face nearest the mirrored spot, on either side.
			const Sint16 *nextFace = std::lower_bound(rowFirst, rowEnd, (Sint16)mirroredPos);
			int bestDistance = 17; // (no further than a tile from the mirrored spot)
			if (nextFace != rowEnd && *nextFace - mirroredPos < bestDistance) { bestPos = *nextFace; bestDistance = *nextFace - mirroredPos; }
			if (nextFace != rowFirst && mirroredPos - nextFace[-1] < bestDistance) bestPos = nextFace[-1];
		}
		if (bestPos < 0) continue;

		// That voxel's colour as it is now: the colour the art gives its open east / south face
		// (inside the craft, only from the same kind of tile part as the hit; never black).
		int sourceVoxel[3] = { worldVoxel[0], worldVoxel[1], worldVoxel[2] };
		sourceVoxel[axis] = bestPos;
		sourceVoxel[1 - axis] = mirror.lo[1 - axis] + rowAcross;
		sourceVoxel[2] = mirror.lo[2] + rowUp;
		// (Never a door's or a curtain's look, a wall one goes through: a player, GitHub issue #147, XPiratez's
		// temple, mirrored as a craft is, wore its curtain's red on the solid wall opposite, "OH A DOOR!!!....wait".)
		const Uint8 color = voxelColorAt(sourceVoxel[0], sourceVoxel[1], sourceVoxel[2], nextFrame, onSkin ? -1 : hit.part, true); // (0 if it has been blown away since)
		if (color && !isBlack(color)) return color;
	}
	return 0;
}

/**
 * Setting "Wall paint" (a player, GitHub issue #111): for each wall, which side its face the
 * isometric art never shows looks onto, and which wall lends that face the paint of that side.
 *
 * The art shows a west wall from the east and a north wall from the south only, the room with its
 * roof cut away: so a building's west and north outer walls are drawn in the paint of the rooms
 * they close, its east and south ones in its outside paint (the city's brick house, beige inside;
 * TFTD's island houses, pink plaster inside, white siding out). In 3D both faces of a wall show
 * the one paint (see colorHiddenSides): from behind a house its rooms' beige, inside a room the
 * brick of its front. Once per battle (with chooseGround, which knows the levels under rock):
 * - each level's tiles fall into regions, the tiles reached from each other without crossing a
 *   wall (a west or north wall part, a door, a window, a big object's wall along an edge; a big
 *   object that fills its tile is in none). A region that reaches the map's edge, or is mostly
 *   without a roof over it (as indoorsAt counts a roof: a floor or an object on a level above),
 *   is the outdoors; none is under rock. The others are rooms, and rooms touching across walls,
 *   or up and down, are a building;
 * - a wall's tile is where its shown face looks, the tile west (north) of it where its hidden
 *   face does. One whose shown face looks into a room and its hidden face outdoors (a building's
 *   west or north outer wall) takes on its hidden face the paint of the nearest of the building's
 *   outer walls whose shown face looks outdoors; one the other way round (an east or south outer
 *   wall) the paint of the walls of that room that close it to the outdoors (else the building's):
 *   the paint of the same kind of wall, so a barn's outer walls keep their brick inside, not the
 *   wood of its stalls; and, with All rooms, a wall between two rooms the paint of the walls
 *   between rooms of the room it faces;
 * - a donor is a plain wall: no door, no glass, no holes in its art, nearly full height; of the
 *   same kind (west or north: the art shades the two sides differently) first, on the same level,
 *   the nearest. Only the voxels of the wall's own paint take its colours: the colour group most
 *   of the nearest plain wall on the same side shows (a window's frame, its glass and curtains,
 *   painted panes, trims keep theirs). Doors keep theirs whole, and so do fences, ghosts, the
 *   walls of crafts and UFOs (mirrored, see mirroredColor) and walls with no donor.
 * Coordinates are tiles. A wall blown away later leaves its rubble as the art has it (see
 * wallPaintColor), and the rest keep theirs: the regions stay as they were found (a saved battle
 * loaded finds them anew, from the map as it is then: a room blown open is the outdoors then).
 */
void FirstPersonView::findWallPaints()
{
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	const int tilesPerLevel = mapSizeX * mapSizeY, tileCount = tilesPerLevel * mapSizeZ;
	_wallPaints.clear();
	_wallPaintOf.assign(tileCount * 2, 0);
	const Uint32 startTick = SDL_GetTicks();
	auto indexOf = [&](int x, int y, int z) { return (z * mapSizeY + y) * mapSizeX + x; };
	auto bigWallOf = [&](int index) { const MapData *object = _save->getTile(index)->getMapData(O_OBJECT); return object ? object->getBigWall() : 0; };

	// A roof over each tile: a floor or an object on any level above (as indoorsAt has it). Each
	// column from the top down.
	std::vector<char> roofed(tileCount, 0);
	for (int column = 0; column < tilesPerLevel; ++column)
	{
		bool roofAbove = false;
		for (int index = column + tileCount - tilesPerLevel; index >= 0; index -= tilesPerLevel)
		{
			roofed[index] = roofAbove;
			const Tile *tile = _save->getTile(index);
			roofAbove = roofAbove || tile->getMapData(O_FLOOR) || tile->getMapData(O_OBJECT);
		}
	}

	// Is the edge between a tile and its neighbour a step (dx, dy) away walled: a west or north
	// wall part on it, or a big object's wall along it (Pathfinding's BIGWALLWEST 4, NORTH 5, EAST
	// 6, SOUTH 7, EASTANDSOUTH 8, WESTANDNORTH 9)? A big object that fills its tile (BIGWALLBLOCK 1,
	// the diagonals 2 and 3) keeps its tile out of every region.
	auto walled = [&](int x, int y, int z, int dx, int dy)
	{
		const int nx = x + dx, ny = y + dy;
		const bool onMap = nx >= 0 && ny >= 0 && nx < mapSizeX && ny < mapSizeY;
		const Tile *tile = _save->getTile(indexOf(x, y, z)), *next = onMap ? _save->getTile(indexOf(nx, ny, z)) : nullptr;
		const int big = bigWallOf(indexOf(x, y, z)), nextBig = onMap ? bigWallOf(indexOf(nx, ny, z)) : 0;
		if (dx == 1) return (next && next->getMapData(O_WESTWALL)) || big == 6 || big == 8 || nextBig == 4 || nextBig == 9;
		if (dx == -1) return tile->getMapData(O_WESTWALL) || big == 4 || big == 9 || nextBig == 6 || nextBig == 8;
		if (dy == 1) return (next && next->getMapData(O_NORTHWALL)) || big == 7 || big == 8 || nextBig == 5 || nextBig == 9;
		return tile->getMapData(O_NORTHWALL) || big == 5 || big == 9 || nextBig == 7 || nextBig == 8;
	};
	auto fillsTile = [&](int index) { const int big = bigWallOf(index); return big >= 1 && big <= 3; };
	static const int Steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	// The regions of each level, by a flood fill from each tile not in one yet, and which of them
	// are the outdoors.
	std::vector<int> region(tileCount, -1);
	std::vector<char> outdoors;
	std::vector<int> stack;
	for (int z = 0; z < mapSizeZ; ++z)
	{
		const bool underRockHere = _enclosed || z < _surfaceLevel;
		for (int y0 = 0; y0 < mapSizeY; ++y0)
			for (int x0 = 0; x0 < mapSizeX; ++x0)
			{
				const int seed = indexOf(x0, y0, z);
				if (region[seed] >= 0 || fillsTile(seed)) continue;
				const int id = (int)outdoors.size();
				region[seed] = id;
				stack.assign(1, seed);
				int cells = 0, unroofed = 0;
				bool reachesEdge = false;
				while (!stack.empty())
				{
					const int index = stack.back();
					stack.pop_back();
					++cells;
					unroofed += !roofed[index];
					const int x = index % mapSizeX, y = (index / mapSizeX) % mapSizeY;
					for (const auto &step : Steps)
					{
						if (walled(x, y, z, step[0], step[1])) continue;
						const int nx = x + step[0], ny = y + step[1];
						if (nx < 0 || ny < 0 || nx >= mapSizeX || ny >= mapSizeY)
						{
							reachesEdge = true;
							continue;
						}
						const int next = indexOf(nx, ny, z);
						if (region[next] >= 0 || fillsTile(next)) continue;
						region[next] = id;
						stack.push_back(next);
					}
				}
				outdoors.push_back(!underRockHere && (reachesEdge || unroofed * 2 > cells));
			}
	}
	auto indoors = [&](int index) { return index >= 0 && region[index] >= 0 && !outdoors[region[index]]; };

	// The buildings: rooms touching across walls, and up and down (a flood fill over the rooms' tiles).
	std::vector<int> building(tileCount, -1);
	int buildingCount = 0;
	for (int seed = 0; seed < tileCount; ++seed)
	{
		if (building[seed] >= 0 || !indoors(seed)) continue;
		building[seed] = buildingCount;
		stack.assign(1, seed);
		while (!stack.empty())
		{
			const int index = stack.back();
			stack.pop_back();
			const int x = index % mapSizeX, y = (index / mapSizeX) % mapSizeY, z = index / tilesPerLevel;
			const int nearby[6][3] = { { x + 1, y, z }, { x - 1, y, z }, { x, y + 1, z }, { x, y - 1, z }, { x, y, z + 1 }, { x, y, z - 1 } };
			for (const auto &cell : nearby)
			{
				if (cell[0] < 0 || cell[1] < 0 || cell[2] < 0 || cell[0] >= mapSizeX || cell[1] >= mapSizeY || cell[2] >= mapSizeZ) continue;
				const int next = indexOf(cell[0], cell[1], cell[2]);
				if (building[next] >= 0 || !indoors(next)) continue;
				building[next] = buildingCount;
				stack.push_back(next);
			}
		}
		++buildingCount;
	}

	// What a wall part can be here: 0 kept as it is (a door, a wall one walks through, a fence, a ghost, nothing to see), 1
	// repainted, 2 repainted and a donor (a plain wall: no glass, no holes in its art, from the
	// floor to nearly the top, its shown face nearly whole), 3 a window (glass or holes in its
	// art): its frame and glass keep theirs, the wall round them may take the paint.
	std::map<const MapData*, int> wallKinds;
	auto wallKind = [&](const MapData *data)
	{
		auto found = wallKinds.find(data);
		if (found != wallKinds.end()) return found->second;
		const VoxelModel *model = getModel(data, -1, false);
		int kind = 0;
		// (A wall one walks through, a curtain, is as a door: it keeps its own and lends none. A player,
		// GitHub issue #147: XPiratez's temple walls wore its curtains' red on their hidden faces, a way
		// through where there is none.)
		const bool walkedThrough = data->getTUCost(MT_WALK) < Pathfinding::INVALID_MOVE_COST;
		if (!data->isDoor() && !data->isUFODoor() && !walkedThrough && !model->empty && !model->ghostly && !model->artFill)
		{
			bool seeThrough = model->openingGlass;
			for (int z = 0; z < Position::TileZ && !seeThrough; ++z)
				for (int y = 0; y < Position::TileXY && !seeThrough; ++y)
					seeThrough = model->glass[z][y] || model->clear[z][y] || model->lookOnly[z][y];
			int shownVoxels = 0;
			for (int z = 0; z < Position::TileZ; ++z)
				for (int along = 0; along < Position::TileXY; ++along)
					shownVoxels += (data->getObjectType() == O_WESTWALL ? model->east[z][along] : model->south[z][along]) != 0;
			kind = seeThrough ? 3 : model->max[2] >= 20 && model->min[2] <= 2 && shownVoxels >= 16 * 20 ? 2 : 1;
		}
		wallKinds[data] = kind;
		return kind;
	};

	// Every wall part: its tile (where its shown face looks), the tile its hidden face looks onto
	// (-1 off the map), and whether each is indoors. Not a craft's or a UFO's (mirrored), nor one
	// beside a big object that fills its tile.
	struct WallSide { int tile, hiddenTile, part; const MapData *data; bool shownIn, hiddenIn; };
	std::vector<WallSide> walls;
	for (int index = 0; index < tileCount; ++index)
	{
		if (!_mirrorOfTile.empty() && _mirrorOfTile[index] >= 0) continue;
		const int x = index % mapSizeX, y = (index / mapSizeX) % mapSizeY, z = index / tilesPerLevel;
		for (int part = O_WESTWALL; part <= O_NORTHWALL; ++part)
		{
			const MapData *data = _save->getTile(index)->getMapData((TilePart)part);
			if (!data || data->getObjectType() != (TilePart)part) continue;
			const int hiddenX = part == O_WESTWALL ? x - 1 : x, hiddenY = part == O_NORTHWALL ? y - 1 : y;
			const int hidden = hiddenX >= 0 && hiddenY >= 0 ? indexOf(hiddenX, hiddenY, z) : -1;
			if (region[index] < 0 || (hidden >= 0 && region[hidden] < 0)) continue;
			walls.push_back({ index, hidden, part, data, indoors(index), indoors(hidden) });
		}
	}

	// The donors, by the kind of wall they are: a building's outer walls whose shown face looks
	// outdoors (its outside paint), by building; its outer walls whose shown face looks into a
	// room (the inside paint of its outer walls), by room and by building; the walls between two
	// rooms (a partition's paint), by the room their shown face looks into.
	std::map<int, std::vector<int>> outsideOf, outerInsideOfRoom, outerInsideOfBuilding, partitionsOfRoom;
	for (int wall = 0; wall < (int)walls.size(); ++wall)
	{
		const WallSide &side = walls[wall];
		if (wallKind(side.data) != 2) continue;
		if (!side.shownIn && side.hiddenIn) outsideOf[building[side.hiddenTile]].push_back(wall);
		if (side.shownIn && !side.hiddenIn)
		{
			outerInsideOfRoom[region[side.tile]].push_back(wall);
			outerInsideOfBuilding[building[side.tile]].push_back(wall);
		}
		if (side.shownIn && side.hiddenIn && region[side.tile] != region[side.hiddenTile]) partitionsOfRoom[region[side.tile]].push_back(wall);
	}
	auto listOf = [](std::map<int, std::vector<int>> &lists, int key) -> const std::vector<int>*
	{
		auto found = lists.find(key);
		return found == lists.end() ? nullptr : &found->second;
	};
	// The nearest wall in a list to another: on the same level first, then the fewest tiles away;
	// of the same kind (west or north) only, or of either. -1 for none.
	auto nearestIn = [&](const WallSide &to, const std::vector<int> *list, bool sameKindOnly)
	{
		int best = -1, bestScore = INT_MAX;
		if (!list) return best;
		const int x = to.tile % mapSizeX, y = (to.tile / mapSizeX) % mapSizeY, z = to.tile / tilesPerLevel;
		for (const int wall : *list)
		{
			const WallSide &from = walls[wall];
			if (sameKindOnly && from.part != to.part) continue;
			const int fromX = from.tile % mapSizeX, fromY = (from.tile / mapSizeX) % mapSizeY, fromZ = from.tile / tilesPerLevel;
			const int score = std::abs(fromZ - z) * 1000 + std::abs(fromX - x) + std::abs(fromY - y);
			if (score < bestScore)
			{
				bestScore = score;
				best = wall;
			}
		}
		return best;
	};
	// The nearest of the same kind from the first list that has one, else of either kind from the
	// first that has any.
	auto nearest = [&](const WallSide &to, std::initializer_list<const std::vector<int>*> lists)
	{
		for (const bool sameKindOnly : { true, false })
			for (const std::vector<int> *list : lists)
			{
				const int found = nearestIn(to, list, sameKindOnly);
				if (found >= 0) return found;
			}
		return -1;
	};

	// Each wall whose two faces look onto different sides: its donor, and the paint of its own side.
	int kept = 0, outerCount = 0, betweenCount = 0;
	for (const WallSide &side : walls)
	{
		if (!wallKind(side.data) || side.hiddenTile < 0) continue;
		int donor = -1, reference = -1; // the wall lending its paint; a plain wall on this wall's own side, for its paint
		Uint8 kind = 0;
		if (side.shownIn && !side.hiddenIn)
		{
			// A building's west or north outer wall, drawn in the room's paint: outdoors, the building's outside.
			kind = 1;
			donor = nearest(side, { listOf(outsideOf, building[side.tile]) });
			reference = nearest(side, { listOf(outerInsideOfRoom, region[side.tile]), listOf(outerInsideOfBuilding, building[side.tile]) });
		}
		else if (!side.shownIn && side.hiddenIn)
		{
			// An east or south outer wall, drawn in the outside paint: inside, that room's outer walls' paint, else the building's.
			kind = 1;
			donor = nearest(side, { listOf(outerInsideOfRoom, region[side.hiddenTile]), listOf(outerInsideOfBuilding, building[side.hiddenTile]) });
			reference = nearest(side, { listOf(outsideOf, building[side.hiddenTile]) });
		}
		else if (side.shownIn && side.hiddenIn && region[side.tile] != region[side.hiddenTile])
		{
			// A wall between two rooms: the paint of the walls between rooms of the room it faces.
			kind = 2;
			donor = nearest(side, { listOf(partitionsOfRoom, region[side.hiddenTile]) });
			reference = nearest(side, { listOf(partitionsOfRoom, region[side.tile]) });
		}
		if (!kind) continue;
		if (donor < 0 || walls[donor].data == side.data || (reference < 0 && wallKind(side.data) == 3) || _wallPaints.size() >= 0xFFFF)
		{
			++kept;
			continue;
		}
		// The paint of the wall's own side: the colour group most of the reference's shown face is
		// in (a wall with none goes by its own).
		const MapData *paintData = reference >= 0 ? walls[reference].data : side.data;
		const bool paintWest = (reference >= 0 ? walls[reference].part : side.part) == O_WESTWALL;
		const VoxelModel *paintModel = getModel(paintData, -1, false);
		int groupCounts[16] = {};
		for (int z = 0; z < Position::TileZ; ++z)
			for (int along = 0; along < Position::TileXY; ++along)
			{
				const Uint8 color = paintWest ? paintModel->east[z][along] : paintModel->south[z][along];
				if (color) ++groupCounts[color >> 4];
			}
		// Does the wall's line end at its north (west) end: no wall of its kind on the tile north (west) of it?
		const int x = side.tile % mapSizeX, y = (side.tile / mapSizeX) % mapSizeY, z = side.tile / tilesPerLevel;
		const int endX = side.part == O_NORTHWALL ? x - 1 : x, endY = side.part == O_WESTWALL ? y - 1 : y;
		const bool openEnd = endX < 0 || endY < 0 || !_save->getTile(indexOf(endX, endY, z))->getMapData((TilePart)side.part);
		_wallPaints.push_back({ getModel(side.data, -1, false), getModel(walls[donor].data, -1, false), walls[donor].part == O_WESTWALL, openEnd, kind,
			(Uint8)(std::max_element(groupCounts, groupCounts + 16) - groupCounts) });
		_wallPaintOf[side.tile * 2 + side.part - O_WESTWALL] = (Uint16)_wallPaints.size();
		if (kind == 1) ++outerCount;
		else ++betweenCount;
	}
	Log(LOG_INFO) << "OpenXcom3D: wall paint: " << buildingCount << " buildings, " << outerCount << " outer walls and " << betweenCount
		<< " walls between rooms take the paint of the side they face (" << kept << " keep theirs), found in " << SDL_GetTicks() - startTick << " ms";
}

/**
 * Setting "Wall paint": the colour a wall's face the isometric art never shows takes from the side
 * it faces (see findWallPaints): the donor's shown face on the same row of voxels, read as one
 * standing before it sees it (its left end on the left), the nearest column of it where it has
 * nothing there. Only on the wall's back plane (its westmost or northmost voxels: the faces inside
 * its thickness, a window opening's jambs, are seen alike from both sides), on the voxels of its
 * own side's paint, and on the wall as the map was built (its rubble keeps its own). A wall's end
 * standing in the other wall's plane (a west wall's north end inside the tile's north wall) is
 * that wall's face; any other end takes the paint at a corner only (the wall's line ends there).
 * @param hit A hit on a face looking west or north.
 * @param tileView The hit tile's snapshot.
 * @param ownColor The colour the face shows as the art has it.
 * @return The colour; 0 to keep its own.
 */
Uint8 FirstPersonView::wallPaintColor(const Hit &hit, const TileView &tileView, Uint8 ownColor) const
{
	if (Options::fpsWallPaint <= 0 || (hit.part != O_WESTWALL && hit.part != O_NORTHWALL) || hit.ghost || !ownColor
		|| hit.axis < 0 || hit.axis > 1 || _wallPaintOf.empty())
	{
		return 0;
	}
	const bool end = hit.axis != hit.part - O_WESTWALL; // a west wall's north end, a north wall's west end
	if (end)
	{
		const int other = O_WESTWALL + O_NORTHWALL - hit.part;
		const VoxelModel *otherWall = tileView.part[other];
		if (otherWall && otherWall->color[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]])
		{
			Hit asOther = hit;
			asOther.part = other;
			return wallPaintColor(asOther, tileView, ownColor);
		}
	}
	const Uint16 paintIndex = _wallPaintOf[hit.tile * 2 + hit.part - O_WESTWALL];
	if (!paintIndex)
	{
		return 0;
	}
	const WallPaint &paint = _wallPaints[paintIndex - 1];
	if (paint.kind > Options::fpsWallPaint || tileView.part[hit.part] != paint.wall || (ownColor >> 4) != paint.paintGroup)
	{
		return 0;
	}
	// On the wall's back plane (its westmost or northmost voxels), or at its very end, at a corner.
	if ((end && !paint.openEnd) || hit.voxel[hit.axis] != paint.wall->min[hit.axis])
	{
		return 0;
	}
	// The donor's column: seen from the west a west wall has north on the left, seen from the north
	// a north wall east; the donor's shown face, seen from the east (south), south (west).
	const int column = hit.part == O_WESTWALL ? (paint.donorWest ? 15 - hit.voxel[1] : hit.voxel[1]) : (paint.donorWest ? hit.voxel[0] : 15 - hit.voxel[0]);
	const Uint8 *row = paint.donorWest ? paint.donor->east[hit.voxel[2]] : paint.donor->south[hit.voxel[2]];
	static const int Nearby[] = { 0, -1, 1, -2, 2, -3, 3 };
	for (const int offset : Nearby)
	{
		const int along = column + offset;
		if (along >= 0 && along < Position::TileXY && row[along]) return row[along];
	}
	return 0;
}

static void turnFromArt(FirstPersonView::VoxelModel &model, const Surface *sprite, int yOffset, int from); // (below)

/**
 * Tells whether an animated record's frames are a light pulsing, blinking or changing its colour, not a thing in
 * motion: such a part always cross-fades softly from frame to frame (see AnimatedNearShades). Two pictures taking
 * turns are (a lift's pad, a blinking light). So are frames whose shades keep their pattern while the colours
 * change: of the pixels two successive frames both draw, in another colour, the shades (a colour's place in its
 * hue's sixteen) rise and fall together (their correlation, AnimatedPulseAlike or more on average). A dance
 * floor's tiles going through eight hues do, pixel for pixel; a piston's band moving up its rod doesn't.
 * @param mapData The tile part's record.
 * @return True for a pulse; false for a part in motion, and for one that doesn't animate.
 */
static bool pulsingFrames(const MapData *mapData)
{
	SurfaceSet *art = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr;
	int pictures[8], count = 0;
	for (int frame = 0; frame < 8; ++frame)
	{
		const int picture = mapData->getSprite(frame);
		if (std::find(pictures, pictures + count, picture) == pictures + count) pictures[count++] = picture;
	}
	if (count <= 2 || !art)
	{
		return count == 2;
	}
	double alike = 0.0;
	int steps = 0;
	for (int frame = 0; frame < 8; ++frame)
	{
		const int now = mapData->getSprite(frame), next = mapData->getSprite((frame + 1) % 8);
		const Surface *from = art->getFrame(now), *to = art->getFrame(next);
		if (now == next || !from || !to || from->getWidth() != to->getWidth() || from->getHeight() != to->getHeight())
		{
			continue;
		}
		// (the two shades of each pixel that changes: sums for their correlation)
		double count2 = 0.0, sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
		for (int y = 0; y < from->getHeight(); ++y)
		{
			for (int x = 0; x < from->getWidth(); ++x)
			{
				const Uint8 before = from->getPixel(x, y), after = to->getPixel(x, y);
				if (!before || !after || before == after) continue;
				const double a = before & 15, b = after & 15;
				count2 += 1.0; sumA += a; sumB += b; sumAA += a * a; sumBB += b * b; sumAB += a * b;
			}
		}
		// (Few pixels changing, or all of one shade going to all of another: nothing moves in that.)
		double correlation = 1.0;
		if (count2 >= 16.0)
		{
			const double varA = sumAA - sumA * sumA / count2, varB = sumBB - sumB * sumB / count2, both = sumAB - sumA * sumB / count2;
			if (varA > 1e-6 && varB > 1e-6) correlation = both / std::sqrt(varA * varB);
		}
		alike += correlation;
		++steps;
	}
	return steps && alike / steps >= AnimatedPulseAlike;
}

/**
 * Gets the voxel colour model of a MapData record (a tile part) shown with a given sprite
 * (animation frame), building it on first use (see buildModel). A view that shares another
 * view's models (the small extra view) asks that view instead.
 * @param mapData The tile part's record.
 * @param spriteIndex The frame of the record's surface set; -1 for its first animation frame.
 * @param cutout Leave out the voxels whose sprite pixel is transparent: used for the frames of
 *        a UFO door sliding open, where the door has gone.
 * @return The model; it stays valid until the battle changes (setSavedBattleGame).
 */
const FirstPersonView::VoxelModel *FirstPersonView::getModel(const MapData *mapData, int spriteIndex, bool cutout)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->getModel(mapData, spriteIndex, cutout);
	}
	if (spriteIndex < 0)
	{
		spriteIndex = mapData->getSprite(0);
	}
	// One key for record, frame and cut-out: the record's address in the high bits, the frame
	// (15 bits) and the cut-out flag (the lowest bit) below it.
	const Uint64 key = ((Uint64)(uintptr_t)mapData << 16) | ((Uint64)(spriteIndex & 0x7FFF) << 1) | (cutout ? 1 : 0);
	auto found = _models.find(key);
	if (found == _models.end())
	{
		// A later frame whose art is the first frame's, pixel for pixel (a mod's wall with two sprite
		// numbers for one picture: XPiratez's NUKE_CITY_2), is the first frame's model: the part
		// doesn't animate at all. As a model of its own it "pulsed" (a player, GitHub issue #157):
		// what goes by the model, a wall's paint on its hidden face (see findWallPaints), held for
		// the first frame only, one tick in eight.
		const int firstSprite = mapData->getSprite(0);
		auto sameArt = _sameArtModels.find(key);
		if (sameArt != _sameArtModels.end())
		{
			return sameArt->second;
		}
		if (spriteIndex != firstSprite)
		{
			SurfaceSet *art = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr;
			const Surface *first = art ? art->getFrame(firstSprite) : nullptr, *frame = art ? art->getFrame(spriteIndex) : nullptr;
			bool same = first && frame && first->getWidth() == frame->getWidth() && first->getHeight() == frame->getHeight();
			for (int y = 0; same && y < first->getHeight(); ++y)
			{
				same = std::equal(first->getRaw(0, y), first->getRaw(0, y) + first->getWidth(), frame->getRaw(0, y));
			}
			if (same)
			{
				const VoxelModel *firstModel = getModel(mapData, firstSprite, cutout);
				_sameArtModels.emplace(key, firstModel);
				return firstModel;
			}
		}
		// Not built yet: build it in place (the map keeps it at the same address from then on). A later frame of a
		// part that animates is built as the kind of part its first frame is (see buildModel).
		// And a part without voxels is, in every frame, the kind of ghost any one of its frames makes it (see partKind).
		const VoxelModel *firstFrame = spriteIndex != firstSprite ? getModel(mapData, firstSprite, cutout) : nullptr;
		if (const VoxelModel *kind = partKind(mapData, cutout)) firstFrame = kind;
		found = _models.emplace(key, VoxelModel()).first;
		buildModel(mapData, spriteIndex, cutout, found->second, firstFrame);
		found->second.pulse = pulsingFrames(mapData);
		// (A thing in motion round its own upright axis, named in the mod's table: this frame's shape turned from this frame's art. See turnedFrom.)
		const int turned = cutout || found->second.empty || found->second.ghostly ? -1 : turnedFrom(mapData);
		if (turned >= 0)
		{
			const Surface *sprite = mapData->getDataset()->getSurfaceset()->getFrame(spriteIndex);
			turnFromArt(found->second, sprite, mapData->getYOffset(), turned);
			if (!found->second.empty) colorHiddenSides(found->second, sprite);
		}
		// The part's own ghost (VoxelModel::ghost): a floor drawn as a box of liquid, its surface
		// (see buildLiquid); a floor or wall whose voxels leave out most of its art, the rest of it
		// (see buildArtFill); an object the art draws as a block of crops, the block (see
		// buildCropBlock).
		if (found->second.liquidHeight || found->second.artFill || found->second.cropBlock)
		{
			VoxelModel &ghost = _ghostModels.emplace(key, VoxelModel()).first->second;
			clearModel(ghost);
			const Surface *sprite = mapData->getDataset()->getSurfaceset()->getFrame(spriteIndex);
			if (found->second.liquidHeight)
			{
				buildLiquid(ghost, sprite, found->second.liquidHeight, mapData->getYOffset());
			}
			else if (found->second.artFill)
			{
				buildArtFill(ghost, found->second, sprite, mapData->getObjectType(), mapData->getYOffset());
			}
			else
			{
				buildCropBlock(ghost, sprite, found->second.cropBlock, mapData->getYOffset());
			}
			found->second.ghost = ghost.empty ? nullptr : &ghost;
		}
	}
	return &found->second;
}

/**
 * The kind of ghost a part that animates and has no voxels is drawn as, in all its frames: what
 * the first of its frames that gets a ghost at all is made (low art standing up, a block, a plain
 * ghost: see buildModel), read off that frame's art. A flame's frames differ enough that some
 * were low art, drawn solid, some a see-through ghost and some nothing: XPiratez's campfire
 * stood as a yellow pillar, then as red streaks, then vanished (a player, GitHub issue #134).
 * Worked out once for a record and kept.
 * @return A model holding the kind (its ghostKind, reliefRises and cropBlock; nothing else of it
 *         is meant to be drawn), or null for a part that doesn't animate or has voxels of its own.
 */
const FirstPersonView::VoxelModel *FirstPersonView::partKind(const MapData *mapData, bool cutout)
{
	bool animates = false;
	for (int frame = 1; frame < 8 && !animates; ++frame) animates = mapData->getSprite(frame) != mapData->getSprite(0);
	if (!animates)
	{
		return nullptr;
	}
	const Uint64 key = ((Uint64)(uintptr_t)mapData << 1) | (cutout ? 1 : 0);
	auto found = _partKinds.find(key);
	if (found == _partKinds.end())
	{
		found = _partKinds.emplace(key, VoxelModel()).first;
		VoxelModel &kind = found->second;
		for (int frame = 0; frame < 8; ++frame)
		{
			buildModel(mapData, mapData->getSprite(frame), cutout, kind);
			if (kind.ghostKind != 0 || !kind.empty)
			{
				break; // (a ghost: that kind; or voxels of its own: it follows its first frame instead, see getModel)
			}
		}
		// A flame (an object that lights its surroundings, by the game's rules, has no voxels, and whose art's outline changes as it burns: a campfire, a torch's
		// fire) is no ghost of any kind: its art stands on the tile as a sprite, each frame flowing into the next, as
		// the game's own fire does (see drawFire). With "Parts without shape" off it isn't drawn, as any such part.
		if (kind.empty || kind.ghostKind != 0)
		{
			// A flame's outline changes from frame to frame. A machine with lights that blink keeps its
			// outline, and is a ghost as any part without shape (a player, GitHub issue #150: the consoles
			// of XPiratez's radar room, which light the room, stood as flat pictures turned to the eye,
			// and hung through the floor seen from below).
			bool outlineChanges = false;
			if (SurfaceSet *art = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr)
			{
				const Surface *first = art->getFrame(mapData->getSprite(0));
				for (int frame = 1; first && frame < 8 && !outlineChanges; ++frame)
				{
					const Surface *other = art->getFrame(mapData->getSprite(frame));
					if (!other || other == first) continue;
					if (other->getWidth() != first->getWidth() || other->getHeight() != first->getHeight())
					{
						outlineChanges = true;
						break;
					}
					for (int y = 0; y < first->getHeight() && !outlineChanges; ++y)
						for (int x = 0; x < first->getWidth() && !outlineChanges; ++x)
							outlineChanges = (first->getPixel(x, y) != 0) != (other->getPixel(x, y) != 0);
				}
			}
			if (outlineChanges && Options::fpsGhostParts != 0 && !cutout && mapData->getObjectType() == O_OBJECT && mapData->getLightSource() > 0)
			{
				clearModel(kind);
				kind.ghostKind = 5;
				_flameParts.insert(mapData);
				Log(LOG_VERBOSE) << "OpenXcom3D: a flame drawn as a sprite: " << mapData->getDataset()->getName() << ", its art's frames " << mapData->getSprite(0) << " to " << mapData->getSprite(7);
			}
		}
	}
	const VoxelModel &kind = found->second;
	return kind.ghostKind != 0 || kind.empty ? &kind : nullptr;
}

/**
 * Gets the leafy model of a mass of foliage (see foliageArt, carveLeaves): the part's model with
 * holes in it, for a tile where it hangs with no floor under it (a tree's crown). Built on first
 * use, as getModel's.
 * @param mapData The tile part's record.
 * @param spriteIndex The frame of the record's surface set.
 * @return The model; it stays valid until the battle changes.
 */
const FirstPersonView::VoxelModel *FirstPersonView::leafyModel(const MapData *mapData, int spriteIndex)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->leafyModel(mapData, spriteIndex);
	}
	const Uint64 key = ((Uint64)(uintptr_t)mapData << 16) | ((Uint64)(spriteIndex & 0x7FFF) << 1);
	auto found = _leafyModels.find(key);
	if (found == _leafyModels.end())
	{
		found = _leafyModels.emplace(key, VoxelModel()).first;
		buildModel(mapData, spriteIndex, false, found->second);
		carveLeaves(found->second, (unsigned)spriteIndex * 2654435761u);
		colorHiddenSides(found->second, mapData->getDataset()->getSurfaceset()->getFrame(spriteIndex));
	}
	return &found->second;
}

/**
 * Gets the sprite frames of a MapData record: the sprite index of each of its 8 animation
 * frames, the pixel buffer of each (to tell which frame a tile shows by comparing buffers), and
 * whether it animates at all. Looked up once per record and kept; a view that shares another
 * view's models asks that view.
 * @param mapData The tile part's record.
 */
const FirstPersonView::Frames &FirstPersonView::getFrames(const MapData *mapData)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->getFrames(mapData);
	}
	auto found = _frames.find(mapData);
	if (found == _frames.end())
	{
		// First time: read the 8 frames from the record and its surface set. It animates if
		// any frame differs from the first.
		Frames frames;
		frames.animated = false;
		SurfaceSet *surfaceSet = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr;
		for (int frame = 0; frame < 8; ++frame)
		{
			frames.sprite[frame] = mapData->getSprite(frame);
			const Surface *surface = surfaceSet ? surfaceSet->getFrame(frames.sprite[frame]) : nullptr;
			frames.buffer[frame] = surface ? surface->getBuffer() : nullptr;
			frames.animated |= frames.sprite[frame] != frames.sprite[0];
		}
		found = _frames.emplace(mapData, frames).first;
	}
	return found->second;
}

/**
 * Builds the voxel colour model of a MapData record (a tile part) shown with one sprite frame.
 *
 * Which voxels are solid comes from the record's LOFT templates, exactly as the game's line of
 * fire reads them (TileEngine::voxelCheck): a tile is 16x16 voxels across and 24 high, and each
 * of its 12 layers of two voxels has a LOFT template, 16 rows of 16 bits (bit 15 - x for voxel
 * x). Each solid voxel's colour comes from the isometric sprite, at the pixel the voxel
 * projects to (see the comment in the loop). Then the plate tops are fitted, the hidden sides
 * coloured, and the glow of a lamp found.
 * @param mapData The tile part's record.
 * @param spriteIndex The frame of the record's surface set to colour it from.
 * @param cutout Leave out the voxels whose pixel is transparent (a sliding UFO door's frames).
 * @param model Where to build it.
 */
void FirstPersonView::buildModel(const MapData *mapData, int spriteIndex, bool cutout, VoxelModel &model, const VoxelModel *firstFrame) const
{
	clearModel(model);
	model.wayThrough = (mapData->getObjectType() == O_WESTWALL || mapData->getObjectType() == O_NORTHWALL)
		&& (mapData->isDoor() || mapData->isUFODoor() || mapData->getTUCost(MT_WALK) < Pathfinding::INVALID_MOVE_COST);

	// The sprite frame to colour it from; without one it stays empty.
	const Surface *sprite = nullptr;
	if (mapData->getDataset() && mapData->getDataset()->getSurfaceset())
	{
		sprite = mapData->getDataset()->getSurfaceset()->getFrame(spriteIndex);
	}
	if (!sprite)
	{
		return;
	}

	// A UFO part's art may have holes drawn in solid black (the inside of its curved hull, see
	// findHollowPixels): the voxels behind them are painted like their neighbours instead
	// (fillHollowVoxels). The sprite's palette tells black; while it's still all black (the
	// battle not shown yet), the mod's battlescape palette.
	std::vector<char> hollowPixels;
	std::vector<int> hollowVoxels;
	bool hasHollows = false;
	if (_ufoDataSets.count(mapData->getDataset()->getName()))
	{
		const SDL_Palette *spritePalette = const_cast<Surface*>(sprite)->getSurface()->format->palette;
		const SDL_Color *palette = spritePalette && spritePalette->ncolors >= 256 ? spritePalette->colors : nullptr;
		int brightness = 0;
		for (int index = 0; palette && index < 256; ++index) brightness += palette[index].r + palette[index].g + palette[index].b;
		if (!brightness)
		{
			const Palette *modPalette = _game->getMod()->getPalette("PAL_BATTLESCAPE", false);
			palette = modPalette ? modPalette->getColors() : nullptr;
		}
		hasHollows = palette && findHollowPixels(sprite, palette, hollowPixels);
	}

	// The most common drawn (not transparent) colour of the sprite, for voxels that project
	// onto transparent pixels (not counting the holes in the art, unless that's all there is).
	// A sprite with no pixels at all is invisible in the isometric view too, so keep it empty.
	int histogram[256] = {}, holeHistogram[256] = {};
	for (int spriteY = 0; spriteY < sprite->getHeight(); ++spriteY)
	{
		for (int spriteX = 0; spriteX < sprite->getWidth(); ++spriteX)
		{
			int *counts = hasHollows && hollowPixels[spriteY * sprite->getWidth() + spriteX] ? holeHistogram : histogram;
			counts[sprite->getPixel(spriteX, spriteY)]++;
		}
	}
	Uint8 fallbackColor = (Uint8)(std::max_element(histogram + 1, histogram + 256) - histogram);
	if (histogram[fallbackColor] == 0)
	{
		fallbackColor = (Uint8)(std::max_element(holeHistogram + 1, holeHistogram + 256) - holeHistogram);
		if (holeHistogram[fallbackColor] == 0)
		{
			return;
		}
	}

	// The record's number in its data set: the mod's tables (windows, shapes) go by it.
	MapDataSet *dataSet = mapData->getDataset();
	int record = -1;
	for (size_t index = 0; index < dataSet->getSize() && record < 0; ++index)
	{
		if (dataSet->getObject(index) == mapData) record = (int)index;
	}

	// Is this record a window (declared by the mod, by data set and record number, and shaped
	// like one, see looksLikeWindow), and which of its colours are glass?
	const std::vector<Uint16> *loftData = _game->getMod()->getVoxelData();
	const RuleFirstPerson &firstPerson = _game->getMod()->getFirstPerson();
	const std::bitset<256> *glassColors = nullptr;
	if (const RuleFirstPerson::Windows *windows = firstPerson.getWindows(dataSet->getName()))
	{
		if (windows->records.count(record) && looksLikeWindow(mapData, *loftData)) glassColors = &windows->glass;
	}

	// A record the game gives no voxels may have a shape in the mod's table (`firstPerson:
	// shapes:`). One that isn't a ghost (a floor: TFTD's sea, which showed the ground outside the
	// map through it) is built here as if the game had given it those voxels, its rows of voxels
	// standing in for the LOFT templates' (bit 15 - x for x, as theirs); a ghost after the loop.
	const RuleFirstPerson::Shape *shape = hasNoVoxels(mapData, *loftData) ? firstPerson.getShape(dataSet->getName(), record, Options::getActiveMaster()) : nullptr;
	std::array<std::array<Uint16, Position::TileXY>, Position::TileZ> shapeRows = {};
	const bool solidShape = shape && !shape->ghost && !cutout;
	// The sea (the mod's water floors) is drawn as waves on its top (see waterColor), and whole:
	// its shape isn't cut to the art, whose few transparent pixels at a tile's corner left holes
	// through to the bottom of the map.
	const bool waterFloor = mapData->getObjectType() == O_FLOOR && firstPerson.isWater(dataSet->getName(), record);
	if (solidShape)
	{
		for (const auto &box : shape->boxes)
			for (int z = box[2]; z <= box[5]; ++z)
				for (int y = box[1]; y <= box[4]; ++y)
					for (int x = box[0]; x <= box[3]; ++x) shapeRows[z][y] |= (Uint16)(1 << (15 - x));
	}

	// A wall's or door's art with holes in it (a glass door's panes, a fence's gaps, see
	// findArtHoles): the voxels behind them are seen through. Not a UFO's walls, whose art's
	// holes are painted over (see findHollowPixels), nor objects (big things drawn tile by tile).
	std::vector<char> artHoles;
	const bool wallArt = (mapData->getObjectType() == O_WESTWALL || mapData->getObjectType() == O_NORTHWALL) && !hasHollows
		&& !_ufoDataSets.count(dataSet->getName());
	const bool hasArtHoles = wallArt && findArtHoles(sprite, artHoles); // (the holes, none or some, are found for any such wall)
	// A pane of glass (see paneOfGlass): the voxels behind its pane are window glass, a smooth
	// see-through tint of what lies behind, its frame solid.
	std::vector<char> pane;
	bool paintedPane = false;
	const bool glassPane = wallArt && !cutout && paneOfGlass(mapData, sprite, artHoles, *loftData, getPalette(), pane, paintedPane); // (painted glass has no holes)
	// On the game's own voxels a pane is glass only where its window is (see paneHoleSpans): the art
	// round it (a shop's wall round its small window) stays solid. Painted glass has no holes: its
	// whole pane is glass.
	std::vector<char> windowGlass;
	if (glassPane && !paintedPane)
	{
		paneHoleSpans(sprite, artHoles, windowGlass);
	}
	const std::vector<char> &glassPixels = paintedPane ? pane : windowGlass;

	// Go through the tile's voxels, layer by layer from the bottom (z), row by row from the
	// north (y), west to east (x).
	const int yOffset = mapData->getYOffset();
	const int nudgeCount = cutout ? 3 : (int)(sizeof(SpriteNudges) / sizeof(SpriteNudges[0]));
	for (int z = 0; z < Position::TileZ; ++z)
	{
		// The LOFT template of this voxel's layer (each template covers two voxel layers).
		const size_t loftStart = mapData->getLoftID(z / 2) * 16;
		if (!solidShape && loftStart + 16 > loftData->size())
		{
			continue;
		}
		for (int y = 0; y < Position::TileXY; ++y)
		{
			const Uint16 loftRow = solidShape ? shapeRows[z][y] : (*loftData)[loftStart + y];
			if (!loftRow)
			{
				continue;
			}
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (!(loftRow & (1 << (15 - x))))
				{
					continue;
				}
				// Where this voxel lands in the 32x40 sprite. The art is 2:1 dimetric: the
				// tile's floor is a diamond 32 pixels wide and 16 high at the bottom of the
				// sprite, its top corner (voxel 0, 0, 0: the tile's north-west corner) at
				// pixel (16, 24). One voxel east (+x) is one pixel right and half a pixel down,
				// one voxel south (+y) one pixel left and half a pixel down, one voxel up (+z)
				// one pixel up. The map draws this sprite yOffset pixels higher than usual, so
				// the voxel lands that many rows further down in it. A line of voxels going
				// east, south and up together lands on the same pixel, so they all take the
				// colour of the front one, the one the art shows (hence colorHiddenSides).
				const int spriteX = std::clamp(16 + x - y, 0, sprite->getWidth() - 1);
				const int spriteY = 24 + (x + y) / 2 - z + yOffset;

				// The pixel's colour, or if the art left it transparent the nearest drawn pixel
				// up or down the column (a pixel outside the sprite counts as transparent).
				Uint8 color = 0;
				int nudge = 0;
				for (; nudge < nudgeCount; ++nudge)
				{
					color = sprite->getPixel(spriteX, spriteY + SpriteNudges[nudge]);
					if (color) break;
				}
				if (solidShape && !waterFloor && !sprite->getPixel(spriteX, spriteY))
				{
					continue; // a shape the mod gave keeps to the art's outline (see buildShape)
				}
				if (!color)
				{
					if (cutout) continue; // that part of the door has slid away
					color = fallbackColor;
				}
				else if (hasHollows && hollowPixels[(spriteY + SpriteNudges[nudge]) * sprite->getWidth() + spriteX])
				{
					hollowVoxels.push_back((z * 16 + y) * 16 + x); // a hole in the art: painted after
				}

				// Store the voxel: its colour, whether it is window glass or behind a hole in the art,
				// and the bounding box.
				model.color[z][y][x] = color;
				if (glassColors && glassColors->test(color))
				{
					model.glass[z][y] |= (Uint16)(1 << x), _anyGlass = true;
				}
				if (glassPane && spriteY >= 0 && spriteY < sprite->getHeight() && glassPixels[spriteY * sprite->getWidth() + spriteX])
				{
					model.glass[z][y] |= (Uint16)(1 << x), _anyGlass = true;
				}
				else if (hasArtHoles && spriteY >= 0 && spriteY < sprite->getHeight() && artHoles[spriteY * sprite->getWidth() + spriteX])
				{
					model.clear[z][y] |= (Uint16)(1 << x);
				}
				model.empty = false;
				const int voxel[3] = { x, y, z };
				for (int axis = 0; axis < 3; ++axis)
				{
					model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
					model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
				}
			}
		}
	}

	// A pane's openings in the game's shape (a house's window has no voxels where its glass is:
	// sight and shots go through) filled with glass too, a look only (see fillPaneOpenings).
	if (glassPane)
	{
		fillPaneOpenings(mapData, sprite, pane, fallbackColor, model);
	}

	// What a pane of glass breaks into: its edges stay in the frame (see addBrokenPaneEdges).
	if (!cutout)
	{
		addBrokenPaneEdges(mapData, model);
	}

	// A wall or an object whose shape lies wholly inside a floor's slab (the two bottom rows of voxels: the remains
	// of a fence, a tuft of grass, a pond, a stump, a grate glowing in a deck): on a floor its voxels are the floor's
	// own, and the rays meet the floor's first, so it showed only where it had no floor under it; the map draws it
	// over the floor. (Found by the developer's arena, see arenaStep: every one of them laid on a floor showed as
	// nothing.) With the setting "Parts without shape" on at all it is made from its art as a part without a shape
	// is whose art is low (see buildRelief): what lies in the floor's diamond laid on the slab, what rises from it
	// standing up. If its art is nowhere near the ground (sparks in the air over a plate) nothing is made: where it
	// animates, it is drawn as a sprite (see artOverlay); where it doesn't, the game's shape is kept.
	const bool inSlab = !model.empty && !cutout && !shape && !glassPane && Options::fpsGhostParts != 0 && mapData->getObjectType() != O_FLOOR
		&& model.max[2] < LowArtFlatLayer;
	VoxelModel slabModel;
	if (inSlab)
	{
		slabModel = model;
		clearModel(model);
		model.wayThrough = slabModel.wayThrough;
	}

	// No voxels at all, though the map draws it (a ladder, a lift's pad, a wall of laser beams):
	// with the setting "Parts without shape" (1: lifts and ladders, the parts with the grav lift
	// flag, cages, as round a lift, and low art, as crops, see lowArt; 2: all of them), a
	// see-through ghost of its art (low art standing up as the map draws it, see buildRelief), with
	// the sides the art never shows coloured; no plate fitting, no glow.
	// A ghost shape from the mod's table (the shops' racks, see buildShape) is built instead, with
	// the setting on at all. Walkable or not, every ghost shows at the setting "Ghost opacity" (the
	// owner took back a fainter ghost where no one can walk: the racks looked far too ghostly).
	if (model.empty)
	{
		const bool ghostShape = shape && shape->ghost;
		bool rises = false;
		bool low = lowArt(sprite, yOffset, rises) && mapData->getObjectType() == O_OBJECT;
		low = low || inSlab; // (a shape in the floor's slab is low, whatever its art)
		int block = cutout ? 0 : cropBlockOf(nullptr, mapData, sprite, yOffset);
		// A floor without shape lying flat that is no lift's pad (a pool's water in XPiratez: no voxels,
		// no floor to stand on): the map draws it whole, so it fills its tile here too, solid, with
		// the setting on at all (a player, GitHub issue #142: half its pixels showed, "loose tiles of
		// water"; with lifts and ladders only, nothing of it, a pit). A lift's pad stays see-through:
		// who is above or below it is a threat to watch.
		bool flatFloor = mapData->getObjectType() == O_FLOOR && !mapData->isGravLift() && !ghostShape && !cageArt(sprite, O_FLOOR, yOffset) && artLiesFlat(sprite, yOffset);
		// (An object that lights its surroundings shows with Lifts too, as its light does: a console, a
		// lamp without shape. A flame among them isn't built here: see partKind.)
		const bool lit = mapData->getObjectType() == O_OBJECT && mapData->getLightSource() > 0;
		// An object the game lets no one walk through, its art big: a body made from the art, solid, with
		// the setting on at all (see artBody).
		bool body = !ghostShape && !low && !block && artBody(mapData, sprite);
		bool ghostWanted = Options::fpsGhostParts == 2
			|| (Options::fpsGhostParts == 1 && (ghostShape || mapData->isGravLift() || cageArt(sprite, mapData->getObjectType(), yOffset) || low || block || flatFloor || lit || body));
		// A part that animates is what its first frame is, in every frame: what kind of ghost it gets is read off the
		// art (how low it is, how like a block or a cage), and a frame whose art falls just the other side of a line
		// (a fireplace's flames, one frame in four; a player, GitHub issue #134) lost its ghost, or stood as another kind.
		if (firstFrame && firstFrame->empty == false && firstFrame->ghostKind == 0)
		{
			firstFrame = nullptr; // (the first frame has voxels of its own, this one's art none: nothing to follow)
		}
		if (firstFrame && firstFrame->ghostKind == 5)
		{
			return; // (a flame: no model, its art is drawn as a sprite; see partKind)
		}
		if (firstFrame)
		{
			ghostWanted = firstFrame->ghostKind != 0;
			block = firstFrame->ghostKind == 2 ? std::max<int>(1, block ? block : firstFrame->cropBlock) : 0;
			low = firstFrame->ghostKind == 3;
			if (low) rises = firstFrame->reliefRises;
			flatFloor = firstFrame->ghostKind == 6;
			body = firstFrame->ghostKind == 7;
		}
		if (ghostWanted && !cutout)
		{
			if (ghostShape)
			{
				model.ghostKind = 1;
				buildShape(model, sprite, *shape, yOffset);
			}
			else if (block)
			{
				model.ghostKind = 2;
				model.cropBlock = (Uint8)block; // (kept for the frames after the first: see above)
				buildCropBlock(model, sprite, block, yOffset);
			}
			else if (low)
			{
				model.ghostKind = 3;
				model.reliefRises = rises;
				buildRelief(model, sprite, yOffset, rises);
			}
			else if (body)
			{
				model.ghostKind = 7;
				buildArtBody(model, sprite, yOffset);
			}
			else
			{
				model.ghostKind = flatFloor ? 6 : 4;
				buildGhost(model, sprite, mapData->getObjectType(), yOffset);
				if (flatFloor) model.solidGhost = !model.empty;
			}
			if (!model.empty) colorHiddenSides(model, sprite);
		}
		if (inSlab && model.empty)
		{
			bool animated = false;
			for (int frame = 1; frame < 8; ++frame) animated = animated || mapData->getSprite(frame) != mapData->getSprite(0);
			if (!animated) model = slabModel;
		}
		return;
	}

	// The voxels behind holes in the art, painted like their neighbours.
	if (!hollowVoxels.empty())
	{
		fillHollowVoxels(model, hollowVoxels, fallbackColor);
	}

	// A window's glass only inside its panes, not on its frame or the wall's edges.
	if (glassColors)
	{
		keepGlassInPanes(model, mapData->getObjectType());
	}

	// An object that is a flat picture on the tile's west or north edge (a poster: thinner than a
	// wall, upright, XPiratez's pin-ups) stands GhostWallClearance voxels into the tile: on the edge
	// it is inside the voxels of the wall it hangs on, which the rays meet first from either side,
	// so it showed only once the wall was shot away (a player, GitHub issue #138; the map draws the
	// object over the wall). Its colours are those its own place gave it. The game's own voxels
	// for it stay where they are: the shift is the view's.
	if (mapData->getObjectType() == O_OBJECT && !model.empty && !cutout && !glassColors && model.max[2] - model.min[2] >= PosterMinExtent)
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			const int other = 1 - axis;
			if (model.max[axis] >= GhostWallClearance || model.max[other] - model.min[other] < PosterMinExtent) continue;
			for (int z = 0; z < Position::TileZ; ++z)
			{
				for (int y = Position::TileXY - 1; y >= 0; --y)
				{
					if (axis == 0)
					{
						for (int x = GhostWallClearance - 1; x >= 0; --x)
						{
							model.color[z][y][x + GhostWallClearance] = model.color[z][y][x];
							model.color[z][y][x] = 0;
						}
						for (Uint16 *bits : { &model.glass[z][y], &model.lookOnly[z][y], &model.clear[z][y] }) *bits = (Uint16)(*bits << GhostWallClearance);
					}
					else if (y < GhostWallClearance)
					{
						for (int x = 0; x < Position::TileXY; ++x)
						{
							model.color[z][y + GhostWallClearance][x] = model.color[z][y][x];
							model.color[z][y][x] = 0;
						}
						for (auto rows : { model.glass, model.lookOnly, model.clear })
						{
							rows[z][y + GhostWallClearance] = rows[z][y];
							rows[z][y] = 0;
						}
					}
				}
			}
			model.min[axis] += GhostWallClearance;
			model.max[axis] += GhostWallClearance;
			break;
		}
	}

	// A thin plate (all its voxels in the bottom 4 layers) may be drawn as a raised slab: fit its top.
	if (!model.empty && model.max[2] <= 3 && !cutout)
	{
		fitPlateTopToSprite(model, sprite, yOffset, nudgeCount);
	}

	// A floor drawn as a box of liquid that slows walking (XPiratez's sewer sludge, see
	// liquidHeightOf): the game's thin slab stays the floor, its top (the bottom under the liquid)
	// a few shades darker; the liquid's surface is a ghost model of its own (see getModel).
	const int walkCost = mapData->getTUCost(MT_WALK);
	// (As for a part without voxels: one that animates is in every frame what its first frame is, a floor of liquid, a
	// part with the rest of its art as a ghost, a block of crops, a mass of foliage; see firstFrame above.)
	if (firstFrame && (firstFrame->empty || firstFrame->ghostKind != 0))
	{
		firstFrame = nullptr;
	}
	if (mapData->getObjectType() == O_FLOOR && !model.empty && model.max[2] <= 3 && !cutout
		&& walkCost > PlainFloorWalkCost && walkCost < Pathfinding::INVALID_MOVE_COST)
	{
		model.liquidHeight = firstFrame ? firstFrame->liquidHeight : (Uint8)liquidHeightOf(sprite, yOffset);
		// (with "Parts without shape" off, no surface: the floor as it is)
		for (int y = 0; model.liquidHeight && Options::fpsGhostParts != 0 && y < Position::TileXY; ++y)
		{
			for (int x = 0; x < Position::TileXY; ++x)
			{
				Uint8 &color = model.color[model.max[2]][y][x];
				if (color) color = (Uint8)((color & 0xF0) | std::min(15, (color & 15) + LiquidBottomShades));
			}
		}
	}

	// A floor or wall whose voxels leave out most of its art (a catwalk's grating, a railing, a
	// chain-link fence: see sparseArt): the rest of its art is a ghost model of its own (see getModel).
	// Not a door, nor a door's other version (an open one: its art shows its leaves swung open, which
	// would stand as a ghost across the doorway; TFTD's liner).
	if (!model.liquidHeight && !cutout && !mapData->isDoor() && !mapData->isUFODoor() && mapData->getAltMCD() == 0)
	{
		model.artFill = firstFrame ? firstFrame->artFill : sparseArt(model, sprite, mapData->getObjectType(), yOffset);
	}
	// An object the art draws as a block of crops though the game gives it only a few voxels
	// (X-Com Files' wheat: see cropBlockOf): the block is a ghost model of its own (see getModel).
	if (!model.empty && !cutout)
	{
		model.cropBlock = firstFrame ? firstFrame->cropBlock : (Uint8)cropBlockOf(&model, mapData, sprite, yOffset);
	}
	// A mass of foliage: drawn leafy on a tile without a floor (see foliageArt, leafyModel).
	model.foliage = firstFrame ? firstFrame->foliage : mapData->getObjectType() == O_OBJECT && !cutout && foliageArt(model, sprite, getPalette());
	colorHiddenSides(model, sprite);
	findGlow(model);
	model.water = waterFloor && !model.empty;
}

/**
 * Is the glass in the openings of a tile part's window broken (see breakGlassAt)? The battle keeps
 * the list (SavedBattleGame::getBrokenGlass), so it's kept in the save.
 * @param tileIndex The tile's index in the map.
 * @param part Its part.
 * @return True if broken.
 */
bool FirstPersonView::glassBroken(int tileIndex, int part) const
{
	const std::vector<int> &broken = _save->getBrokenGlass();
	return std::binary_search(broken.begin(), broken.end(), tileIndex * 4 + part);
}

/**
 * What is left of a sliding door once it has slid open (a UFO's door, a mod's electric door:
 * the last frame's picture, cut out where the door has gone), as a ghost: seen through and
 * never in the way, as any part the game gives no shape (setting "Parts without shape"). The
 * Setting "Open doors as ghosts". The
 * game has an open door as no obstacle and nothing to see past, and what the picture still
 * shows of it stood solid in the doorway (a player, GitHub issue #165: "any voxels without
 * hitbox should become ghost when open, to prevent visibility hampering"). Made the first time
 * it's needed, kept as long as the models.
 * @param model The open door's model.
 * @return The same voxels as a ghost.
 */
const FirstPersonView::VoxelModel *FirstPersonView::openDoorModel(const VoxelModel *model)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->openDoorModel(model);
	}
	auto made = _openDoorModels.find(model);
	if (made == _openDoorModels.end())
	{
		made = _openDoorModels.emplace(model, *model).first;
		made->second.ghostly = true;
		if (!made->second.ghostKind) made->second.ghostKind = 4; // (a plain ghost)
	}
	return &made->second;
}

/**
 * A model with the glass filled into its openings (VoxelModel::lookOnly, see fillPaneOpenings)
 * broken out, as a round going through left it (see breakGlassAt): the opening empty, as the
 * game has it (and as 1.09 showed it). Made the first time it's needed, and kept as long as the
 * models (a view that shares another's models shares these too).
 * @param model The whole model.
 * @return The model without its opening's glass.
 */
const FirstPersonView::VoxelModel *FirstPersonView::openedModel(const VoxelModel *model)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->openedModel(model);
	}
	auto made = _openedModels.find(model);
	if (made == _openedModels.end())
	{
		made = _openedModels.emplace(model, *model).first;
		VoxelModel &opened = made->second;
		// The opening's glass voxels gone; the bounding box and emptiness again from what's left.
		opened.openingGlass = false;
		opened.empty = true;
		for (int axis = 0; axis < 3; ++axis)
		{
			opened.min[axis] = 255;
			opened.max[axis] = 0;
		}
		for (int z = 0; z < Position::TileZ; ++z)
		{
			for (int y = 0; y < Position::TileXY; ++y)
			{
				const Uint16 broken = opened.lookOnly[z][y];
				opened.glass[z][y] &= (Uint16)~broken;
				opened.lookOnly[z][y] = 0;
				for (int x = 0; x < Position::TileXY; ++x)
				{
					if ((broken >> x) & 1) opened.color[z][y][x] = 0;
					if (!opened.color[z][y][x]) continue;
					opened.empty = false;
					const int voxel[3] = { x, y, z };
					for (int axis = 0; axis < 3; ++axis)
					{
						opened.min[axis] = (Uint8)std::min<int>(opened.min[axis], voxel[axis]);
						opened.max[axis] = (Uint8)std::max<int>(opened.max[axis], voxel[axis]);
					}
				}
			}
		}
	}
	return &made->second;
}

/**
 * Fills a pane of glass's openings with glass: where the game's shape has no voxel across the
 * wall (a house's window, whose glass the game leaves open, so sight and shots go through) and
 * the art there is inside the pane (see paneOfGlass), glass voxels in the wall's own rows, so the
 * window shows a pane, not a hole; none in a wall thicker than GlassThinRowsMax rows (a plane's
 * fuselage, whose rows would fill into a block) (the owner, after GitHub issue #75: "glass by the game's
 * rules"). They're a look only (VoxelModel::lookOnly): the aim goes through them as the game's
 * rounds do, and they don't break into shards (see addBrokenPaneEdges); a round going through
 * breaks them out, for the rest of the battle (see breakGlassAt).
 * @param mapData The wall's record.
 * @param sprite Its sprite frame.
 * @param pane Its pane (see paneOfGlass).
 * @param fallbackColor A colour for a voxel over a hole in the art (glass is drawn by the tint of
 *        what's behind it, not its colour).
 * @param model Its model, its own voxels already in.
 */
void FirstPersonView::fillPaneOpenings(const MapData *mapData, const Surface *sprite, const std::vector<char> &pane, Uint8 fallbackColor, VoxelModel &model) const
{
	if (model.empty)
	{
		return;
	}
	// The wall's rows across it: y for a north wall, x for a west wall (the ones its voxels are in);
	// none filled in a thick one (a plane's fuselage: its openings stay open).
	const bool westWall = mapData->getObjectType() == O_WESTWALL;
	Uint16 rows = 0;
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
				if (model.color[z][y][x]) rows |= (Uint16)(1 << (westWall ? x : y));
	if ((int)std::bitset<16>(rows).count() > GlassThinRowsMax)
	{
		return;
	}
	const int width = sprite->getWidth(), height = sprite->getHeight(), yOffset = mapData->getYOffset();
	for (int z = 0; z < Position::TileZ; ++z)
	{
		for (int along = 0; along < Position::TileXY; ++along)
		{
			// Only where the wall has no voxel at all across it.
			bool solid = false;
			for (int across = 0; across < Position::TileXY && !solid; ++across)
			{
				solid = model.color[z][westWall ? along : across][westWall ? across : along] != 0;
			}
			if (solid) continue;
			for (int across = 0; across < Position::TileXY; ++across)
			{
				if (!((rows >> across) & 1)) continue;
				const int x = westWall ? across : along, y = westWall ? along : across;
				// Where the voxel lands in the art (as in buildModel): inside the pane?
				const int spriteX = std::clamp(16 + x - y, 0, width - 1);
				const int spriteY = 24 + (x + y) / 2 - z + yOffset;
				if (spriteY < 0 || spriteY >= height || !pane[spriteY * width + spriteX]) continue;
				const Uint8 color = sprite->getPixel(spriteX, spriteY);
				model.color[z][y][x] = color ? color : fallbackColor;
				model.glass[z][y] |= (Uint16)(1 << x), _anyGlass = true;
				model.lookOnly[z][y] |= (Uint16)(1 << x);
				model.openingGlass = true;
				const int voxel[3] = { x, y, z };
				for (int axis = 0; axis < 3; ++axis)
				{
					model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
					model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
				}
			}
		}
	}
}

/**
 * The record a pane of glass turns into when broken (its "die" record, see paneOfGlass) keeps
 * the pane's edges: a jagged band of glass, one to three voxels wide, all round the inside of the
 * frame (and the frame itself), solid, as shards left standing; the middle is open (the owner,
 * GitHub issue #75: "when broken only voxels on the edges shown"). Its own voxels (the shards on
 * the floor) stay. Only a pane, at least half the game's own glass: a wall with a window in it
 * breaks into its rubble alone. The pane's model is built here, not taken from the models kept (that cache may
 * grow while this one is being built).
 * @param mapData The record being built.
 * @param model Its model, its own voxels already in.
 */
void FirstPersonView::addBrokenPaneEdges(const MapData *mapData, VoxelModel &model) const
{
	static thread_local bool searching = false; // (a pane's own model isn't searched again)
	MapDataSet *dataSet = mapData->getDataset();
	if (searching || !dataSet)
	{
		return;
	}
	for (size_t index = 0; index < dataSet->getSize(); ++index)
	{
		const MapData *intact = dataSet->getObject(index);
		if (intact == mapData || intact->getDieMCD() <= 0 || intact->getDieMCD() >= (int)dataSet->getSize()
			|| dataSet->getObject(intact->getDieMCD()) != mapData || intact->getObjectType() != mapData->getObjectType())
		{
			continue;
		}
		// The intact pane's model; glass in it means it was a pane.
		searching = true;
		auto paneModel = std::make_unique<VoxelModel>();
		buildModel(intact, intact->getSprite(0), false, *paneModel);
		searching = false;
		// A pane: most of it the game's own glass (not glass filled into an opening, a look only).
		// A wall with a window in it is no pane: what it breaks into is the game's rubble and
		// nothing more (a player, GitHub issue #149: XPiratez's temple walls shot down stood on as
		// frames round a hole, the whole wall but its window, where the game has a low stump and
		// rounds pass).
		int glassVoxels = 0, allVoxels = 0;
		for (int z = 0; z < Position::TileZ; ++z)
		{
			for (int y = 0; y < Position::TileXY; ++y)
			{
				for (int x = 0; x < Position::TileXY; ++x)
				{
					if (!paneModel->color[z][y][x] || ((paneModel->lookOnly[z][y] >> x) & 1)) continue;
					++allVoxels;
					if ((paneModel->glass[z][y] >> x) & 1) ++glassVoxels;
				}
			}
		}
		if (paneModel->empty || glassVoxels * 2 < allVoxels || !glassVoxels)
		{
			continue;
		}
		// Along the wall: y for a west wall, x for a north wall. A glass voxel stays within its
		// band of the pane's edge (1 to 3 voxels, by a fixed scatter), a frame voxel always.
		const bool westWall = mapData->getObjectType() == O_WESTWALL;
		const int alongMin = paneModel->min[westWall ? 1 : 0], alongMax = paneModel->max[westWall ? 1 : 0];
		for (int z = 0; z < Position::TileZ; ++z)
		{
			for (int y = 0; y < Position::TileXY; ++y)
			{
				for (int x = 0; x < Position::TileXY; ++x)
				{
					const Uint8 color = paneModel->color[z][y][x];
					if (!color || model.color[z][y][x] || ((paneModel->lookOnly[z][y] >> x) & 1)) continue; // (glass filled into an opening isn't the game's: no shards)
					if ((paneModel->glass[z][y] >> x) & 1)
					{
						const int along = westWall ? y : x;
						const int edge = std::min({ along - alongMin, alongMax - along, z - paneModel->min[2], paneModel->max[2] - z });
						if (edge >= BrokenPaneEdgeMin + (along * 7 + z * 13) % (BrokenPaneEdgeExtra + 1)) continue;
					}
					model.color[z][y][x] = color;
					model.empty = false;
					const int voxel[3] = { x, y, z };
					for (int axis = 0; axis < 3; ++axis)
					{
						model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
						model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
					}
				}
			}
		}
		return;
	}
}

/**
 * Notices the doors that just turned on a tile and starts their swing. When the game opens an
 * ordinary door it replaces the door in the wall with its other version (the record's
 * alternative MCD), which stands in the tile's other wall: a door in the north wall turns into
 * the west wall and back, around the tile's north-west corner, its hinge. Both versions are
 * doors (the open key turns an open one back), so the 3D view doesn't tell opening from
 * shutting: a wall that was a door last frame and is something else now, while its other
 * version shows up on the tile, means the door just turned, through the tile, to where the game
 * has it now. UFO doors slide instead (see snapshotTile).
 * @param index The tile's index in the map.
 * @param tile The tile.
 */
void FirstPersonView::trackDoors(int index, Tile *tile)
{
	// The tile's two walls: west (0) and north (1).
	for (int wall = 0; wall < 2; ++wall)
	{
		// What the wall is now and what it was last frame (and remember it for next frame).
		const TilePart part = wall ? O_NORTHWALL : O_WESTWALL;
		const MapData *wallNow = tile->getMapData(part);
		const MapData *wallBefore = _wallsLastFrame[wall][index];
		_wallsLastFrame[wall][index] = wallNow;
		if (!wallBefore || wallBefore == wallNow || !wallBefore->isDoor() || wallBefore->isUFODoor() || !wallBefore->getDataset())
		{
			continue;
		}

		// Look for the door's other version on any part of the tile.
		const MapData *otherDoor = wallBefore->getDataset()->getObject(wallBefore->getAltMCD());
		for (int otherPart = 0; otherPart < 4; ++otherPart)
		{
			if (otherDoor && tile->getMapData((TilePart)otherPart) == otherDoor)
			{
				// The swing: from along the wall it left (north: east, 0; west: south, 90
				// degrees) a quarter turn through the tile to the other wall, as the game turned
				// it. This door's last swing ends: one that brought it into the wall it leaves
				// now (it turned again before that swing ended) hands over the angle it had got
				// to, so the door swings back from there. (Another door of the tile, in its other
				// wall, keeps its own swing.)
				const float quarterTurn = 1.5707963f;
				float fromAngle = part == O_NORTHWALL ? 0.0f : quarterTurn;
				const float toAngle = part == O_NORTHWALL ? quarterTurn : 0.0f;
				const Uint32 now = SDL_GetTicks();
				for (auto old = _doorAnimations.begin(); old != _doorAnimations.end();)
				{
					if (old->tile != index || (old->hiddenPart != (int)part && old->part != (int)part))
					{
						++old;
						continue;
					}
					if (old->hiddenPart == (int)part)
					{
						const float progress = std::min(1.0f, (now - old->start) / (float)DoorSwingTime);
						const float eased = progress * progress * (3.0f - 2.0f * progress);
						fromAngle = old->fromAngle + (old->toAngle - old->fromAngle) * eased;
					}
					old = _doorAnimations.erase(old);
				}
				_doorAnimations.push_back({ index, part, otherPart, getModel(wallBefore), now, fromAngle, toAngle, otherDoor });
				break;
			}
		}
	}
}

/**
 * Is the door in this wall still moving? An ordinary door swings for DoorSwingTime ms after it
 * turned; a UFO door slides through its animation frames for UfoDoorTime ms (one the view has
 * not started sliding yet counts as moving too). The first-person mode waits for it before
 * stepping through the doorway.
 * @param pos The tile.
 * @param part The wall (a TilePart: O_WESTWALL or O_NORTHWALL).
 */
bool FirstPersonView::isDoorMoving(const Position &pos, int part)
{
	Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return false;
	}
	// An ordinary door still swinging.
	const int index = _save->getTileIndex(pos);
	for (const auto &animation : _doorAnimations)
	{
		if (animation.tile == index && animation.part == part && SDL_GetTicks() - animation.start < DoorSwingTime)
		{
			return true;
		}
	}
	// An open UFO door still sliding.
	const MapData *mapData = tile->getMapData((TilePart)part);
	if (mapData && mapData->isUFODoor() && tile->isUfoDoorOpen((TilePart)part))
	{
		auto opening = _ufoDoorOpening.find(index * 4 + part);
		return opening == _ufoDoorOpening.end() || SDL_GetTicks() - opening->second < UfoDoorTime;
	}
	return false;
}

/**
 * Works out what lies past the map's border (see colorOutsideMap), from its outer ring of tiles:
 * - where its surface is: the first level, from the bottom, whose ring isn't underground (at
 *   least UndergroundRingShare of its tiles rock: a wall or an object, the rock a base or a cave
 *   is cut into, or a floor with something over it). On any map outdoors that's ground level; on a map that starts on the surface
 *   and goes on underground, the surface's level; where the sea lies on a level above that (round
 *   a ship's lower decks), the sea's. Beyond the border, below the surface, is rock;
 * - whether the map is underground all the way up (every level of the ring is rock: a base, an
 *   alien colony's inside, Cydonia's complex), or its mission is listed as underground in the
 *   mod (RuleFirstPerson::isUnderground): then rock lies beyond the border at every height and
 *   over the map too, where it would otherwise show the sky (or, under the sea, the water);
 * - the ground that runs on past the border at the surface's level: the most common floor of the
 *   ring there (grass on a farm, sand in the desert, ...). Only a floor whose model reaches down
 *   to the bottom voxel layer counts;
 * - the water: where a floor of the mod's water (RuleFirstPerson::isWater: TFTD's sea, the big
 *   mods' copy of it, the sea round TFTD's ships among their parts) reaches
 *   the border, the sea goes on beyond that stretch of it, the most common water floor of the
 *   ring (an island's shore runs on out of the map, the sea round a ship); the ground runs on
 *   past the rest. A ring of water alone (round a ship) is the ground too. The sea's colours
 *   for its waves (see waterColor) come from that floor's top, or with no water at the edge
 *   from the commonest water floor on the map.
 * In space (a station, a ship: see chooseSky) none of it: only space lies past the map's edge,
 * round it, under it and over it, however walled in its edges are.
 * Sets _surfaceLevel, _enclosed, _groundModel (left as it was if no floor qualifies),
 * _waterModel, _waterAtEdge, _waterColors and _groundChosen; then finds the walls' paints (see
 * findWallPaints), which go by the levels under rock.
 */
void FirstPersonView::chooseGround()
{
	_groundChosen = true;
	chooseSky();
	const bool inSpace = _sky == SkySpace || _sky == SkyOrbit;
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	// Calls a function on each tile of the ring at a level.
	auto forEachRingTile = [&](int z, auto &&visit)
	{
		for (int y = 0; y < mapSizeY; ++y)
		{
			for (int x = 0; x < mapSizeX; ++x)
			{
				if (x == 0 || y == 0 || x == mapSizeX - 1 || y == mapSizeY - 1)
				{
					visit(_save->getTile(Position(x, y, z)));
				}
			}
		}
	};

	// The surface: the levels from the bottom whose ring is rock are underground. A ring tile is
	// rock when it holds a wall or an object, or a floor with anything over it (a base's sewers
	// running out to the edge under the rock).
	_surfaceLevel = 0;
	for (int z = 0; z < mapSizeZ; ++z)
	{
		int ringTiles = 0, rockTiles = 0;
		forEachRingTile(z, [&](const Tile *tile)
		{
			++ringTiles;
			bool rock = tile->getMapData(O_WESTWALL) || tile->getMapData(O_NORTHWALL) || tile->getMapData(O_OBJECT);
			for (int above = z + 1; !rock && tile->getMapData(O_FLOOR) && above < mapSizeZ; ++above)
			{
				const Tile *over = _save->getTile(tile->getPosition() + Position(0, 0, above - z));
				rock = over->getMapData(O_FLOOR) || over->getMapData(O_WESTWALL) || over->getMapData(O_NORTHWALL) || over->getMapData(O_OBJECT);
			}
			if (rock) ++rockTiles;
		});
		if (ringTiles == 0 || rockTiles < ringTiles * UndergroundRingShare)
		{
			break;
		}
		_surfaceLevel = z + 1;
	}
	_enclosed = _surfaceLevel >= mapSizeZ || _save->getMod()->getFirstPerson().isUnderground(_save->getMissionType());
	if (_enclosed)
	{
		_surfaceLevel = mapSizeZ;
	}
	if (inSpace)
	{
		_surfaceLevel = 0;
		_enclosed = false;
	}
	// The sea round a ship's lower decks (TFTD's liner, its second stage) lies on a level above
	// the floors at the edge: the surface is the highest level of the ring that is mostly water.
	for (int z = mapSizeZ - 1; !_enclosed && !inSpace && z > _surfaceLevel; --z)
	{
		int ringTiles = 0, waterTiles = 0;
		forEachRingTile(z, [&](const Tile *tile)
		{
			++ringTiles;
			const MapData *floor = tile->getMapData(O_FLOOR);
			if (floor && getModel(floor)->water) ++waterTiles;
		});
		if (waterTiles * 2 > ringTiles)
		{
			_surfaceLevel = z;
		}
	}
	// The rock's colour: the commonest colour group among the walls and objects round the map's
	// underground levels (the rock it's cut into, in whatever palette the battle has: a fixed
	// brown came out olive or watery blue under the sea), at a middle shade, with room to shade
	// it either way. Every third voxel of their models is enough to tell. A group too dark at that
	// shade to show its hue (below DarkGlowLuma) can't be rock: T'leth's first level is walled
	// round in the deep sea palette's black (group 112, black at every shade), and its ceiling
	// stayed black under night vision and the flashlight (the owner: "How does ceiling look like
	// in final?"); the commonest of the others is taken (the map's own colours still), and with
	// none, the plain rock (see rockColor). A dark rock that shows its hue (XPiratez's sewers,
	// its Thule base) stays as it is.
	_rockBase = 0;
	std::map<int, int> rockGroups;
	for (int z = 0; z < std::min(_surfaceLevel, mapSizeZ); ++z)
	{
		forEachRingTile(z, [&](const Tile *tile)
		{
			for (TilePart part : { O_WESTWALL, O_NORTHWALL, O_OBJECT })
			{
				const MapData *mapData = tile->getMapData(part);
				if (!mapData) continue;
				const VoxelModel *model = getModel(mapData);
				for (int voxelZ = 0; voxelZ < 24; voxelZ += 3)
					for (int voxelY = 0; voxelY < 16; voxelY += 3)
						for (int voxelX = 0; voxelX < 16; voxelX += 3)
							if (const Uint8 color = model->color[voxelZ][voxelY][voxelX]) rockGroups[color & 0xF0]++;
			}
		});
	}
	const SDL_Color *palette = getPalette();
	int rockCount = 0, mostVoxels = 0, mostGroup = -1;
	for (const auto &group : rockGroups)
	{
		const SDL_Color &middle = palette[group.first | RockBaseShade];
		if (group.second > rockCount && middle.r * 0.3f + middle.g * 0.59f + middle.b * 0.11f >= DarkGlowLuma)
		{
			rockCount = group.second;
			_rockBase = (Uint8)(group.first | RockBaseShade);
		}
		if (group.second > mostVoxels)
		{
			mostVoxels = group.second;
			mostGroup = group.first;
		}
	}
	Log(LOG_VERBOSE) << "OpenXcom3D: " << _save->getMissionType() << ": " << (_enclosed ? "underground all the way up (rock round the map and over it)"
		: "the surface at the map's edges is level " + std::to_string(_surfaceLevel) + " (rock beyond the edges below it)")
		<< ", the rock in " << (_rockBase ? "colour group " + std::to_string(_rockBase & 0xF0) : std::string("plain colours"))
		<< (mostGroup >= 0 && mostGroup != (_rockBase & 0xF0) ? " (the commonest, " + std::to_string(mostGroup) + ", too dark)" : std::string());

	// Count the floors of the ring at the surface's level (the bottom one on an enclosed map,
	// which shows no ground beyond the border anyway), the water's apart, and note where the
	// water is (by the tile's x and y).
	std::map<const MapData*, int> floorCounts, waterCounts;
	_waterAtEdge.assign(mapSizeX * mapSizeY, 0);
	forEachRingTile(std::min(_surfaceLevel, mapSizeZ - 1), [&](const Tile *tile)
	{
		if (const MapData *floor = tile->getMapData(O_FLOOR))
		{
			const bool water = getModel(floor)->water;
			(water ? waterCounts : floorCounts)[floor]++;
			_waterAtEdge[tile->getPosition().y * mapSizeX + tile->getPosition().x] = water;
		}
	});

	// Of each, the most common one with a model that lies on the ground (a ghost never does).
	auto commonest = [&](const std::map<const MapData*, int> &counts) -> const VoxelModel*
	{
		const VoxelModel *best = nullptr;
		int bestCount = 0;
		for (const auto &entry : counts)
		{
			const VoxelModel *model = getModel(entry.first);
			if (entry.second > bestCount && !model->empty && !model->ghostly && model->min[2] == 0)
			{
				bestCount = entry.second;
				best = model;
			}
		}
		return best;
	};
	_waterModel = inSpace ? nullptr : commonest(waterCounts);
	if (inSpace)
	{
		_groundModel = nullptr;
	}
	else if (const VoxelModel *ground = commonest(floorCounts))
	{
		_groundModel = ground;
	}
	else if (_waterModel)
	{
		_groundModel = _waterModel; // nothing but water round the map
	}

	// No water at the edge: the commonest water floor on the whole level, for the sea's colours.
	const VoxelModel *seaModel = _waterModel;
	if (!seaModel)
	{
		std::map<const MapData*, int> mapWaterCounts;
		const int level = std::min(_surfaceLevel, mapSizeZ - 1);
		for (int y = 0; y < mapSizeY; ++y)
			for (int x = 0; x < mapSizeX; ++x)
			{
				const MapData *floor = _save->getTile(Position(x, y, level))->getMapData(O_FLOOR);
				if (floor && getModel(floor)->water) mapWaterCounts[floor]++;
			}
		seaModel = commonest(mapWaterCounts);
	}
	chooseWaterColors(seaModel);
	// The walls' faces the art never shows, once the levels under rock are known (setting "Wall paint").
	findWallPaints();
}

/**
 * Picks the sea's colours for its waves (see waterColor) from its floor's top, in whatever
 * palette the battle has: the plain colour (the top's colour nearest their average), the deep
 * colour (the commonest of the top's colours from another colour group that is darker but not
 * too dark to show its hue: TFTD's teal under its sea-green; without one the plain colour three
 * shades darker) and the foam (the plain colour FoamShades lighter). Each is kept WaveShades
 * lighter, as on a crest (as far as its group goes): the wave's shade takes it down.
 * @param seaModel The sea's floor, or null for a map without water (no colours: no waves).
 */
void FirstPersonView::chooseWaterColors(const VoxelModel *seaModel)
{
	_waterColors[0] = _waterColors[1] = _waterColors[2] = 0;
	if (!seaModel || !seaModel->ceiling)
	{
		return;
	}
	// Each colour's brightness to the eye, and a colour so many shades lighter within its group.
	const SDL_Color *palette = getPalette();
	auto luminance = [palette](Uint8 color) { return palette[color].r * 0.3f + palette[color].g * 0.59f + palette[color].b * 0.11f; };
	auto lighter = [](Uint8 color, int shades) { return (Uint8)((color & 0xF0) | std::max(0, (color & 15) - shades)); };
	Uint8 plain = seaModel->ceiling, deep = 0;
	if (luminance(plain) < DarkGlowLuma)
	{
		// A ship's dark floor round its hull standing in for the sea (TFTD's liner draws it black,
		// its cargo ship all but black): the colours of TFTD's sea instead, the palette's nearest.
		plain = nearestColor(palette, StandInSeaPlain);
		deep = nearestColor(palette, StandInSeaDeep);
	}
	else
	{
		// The deep colour: count the top's colours of other groups, darker than the plain one.
		int counts[256] = {};
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = seaModel->plan[y][x];
				if (color && (color & 0xF0) != (plain & 0xF0) && luminance(color) < luminance(plain) && luminance(color) >= DarkGlowLuma) counts[color]++;
			}
		const int deepest = (int)(std::max_element(counts, counts + 256) - counts);
		deep = counts[deepest] ? (Uint8)deepest : (Uint8)((plain & 0xF0) | std::min(13, (plain & 15) + 3));
	}
	const int crest = (int)std::lround(WaveShades);
	_waterColors[0] = lighter(plain, crest);
	_waterColors[1] = lighter(deep, crest);
	_waterColors[2] = lighter(plain, crest + FoamShades);
}

/**
 * Takes this frame's snapshot of the map for the rays (one TileView per tile): which voxel
 * model (and animation frame) sits in each tile part, the light, the fog of war, and the doors
 * swinging open. Since it reads the map afresh every frame, opened doors, destruction and the
 * like show up by themselves; on the way it notices what changed since the last frame (doors
 * opening, fires burning out, parts destroyed) and gathers the light sources and UFO devices
 * near the camera.
 */
void FirstPersonView::snapshotTiles()
{
	// The first frame of a battle (or after a reload, or the map changed size): start the
	// per-tile memories of the last frame afresh; this frame only notes the map as it is,
	// without looking for changes.
	const int tileCount = _save->getMapSizeXYZ();
	const bool trackWalls = (int)_wallsLastFrame[0].size() == tileCount;
	if (!trackWalls)
	{
		_wallsLastFrame[0].assign(tileCount, nullptr);
		_wallsLastFrame[1].assign(tileCount, nullptr);
		for (auto &parts : _partsLastFrame) parts.assign(tileCount, nullptr);
		_burningLastFrame.assign(tileCount, 0);
		_tileMemos.assign(tileCount, TileMemo{ {}, nullptr, {}, nullptr });
		_partMemos.assign(tileCount, std::array<PartMemo, 4>());
		_doorAnimations.clear();
	}
	_tileViews.resize(tileCount);

	// Animated tiles run on the view's own calm clock rather than the game's 100 ms tick (see
	// CalmFrameTime): the frame they show now, and how far (eased in and out) they have
	// dissolved into the next one.
	const Uint32 now = SDL_GetTicks();
	_calmFrame = (int)((now / CalmFrameTime) % 8);
	_animationBlend = smoothstep(0.0f, 1.0f, (now % CalmFrameTime) / (float)CalmFrameTime);
	// UFO parts show their steady models when the UFO lights option is on (see findUfos).
	_ufoLightsOn = Options::fpsUfoLights && !_ufos.empty();

	// Every tile: notice what changed (or start watching it), take its snapshot, and note the
	// light sources and devices on it. Then, while the tile is still at hand, the byte the rays
	// look at first as they walk the map: anything to trace in the tile, smoke on it, and the
	// height of its highest solid voxel (see castRay); and its light, close together with the
	// others' for the shading (see lightAt).
	// Every tile's snapshot is taken afresh on the first frame, and when something changed that
	// any tile's may hang on: the map's debug vision, the UFO lights, the floor left out, parts
	// left out. A tile a door swung through last frame (the swing writes into its snapshot, see
	// updateDoorSwings) has its own taken afresh.
	const bool debug = _save->getDebugMode(), leftOut = !_leftOutParts.empty() || !_ghostedParts.empty();
	const bool everyTile = !trackWalls || debug != _snapshotDebug || _ufoLightsOn != _snapshotUfoLights || _hiddenFloor != _snapshotHiddenFloor
		|| leftOut || _snapshotLeftOut;
	_snapshotDebug = debug;
	_snapshotUfoLights = _ufoLightsOn;
	_snapshotHiddenFloor = _hiddenFloor;
	_snapshotLeftOut = leftOut;
	for (int swung : _swungTiles)
	{
		if (swung < tileCount) _tileMemos[swung].restless = true;
	}
	_swungTiles.clear();
	_nearbyLights.clear();
	_nearbyDevices.clear();
	_tileFlags.resize(tileCount);
	_tileLights.resize(tileCount);
	for (int index = 0; index < tileCount; ++index)
	{
		Tile *tile = _save->getTile(index);
		bool changed = true;
		if (trackWalls)
		{
			changed = noticeTileChanges(index, tile);
		}
		else
		{
			startWatchingTile(index, tile);
		}
		// Tiles hardly ever change: one whose parts are last frame's, and that shows nothing that
		// moves by itself (see TileMemo::restless), keeps its snapshot; only its light is read.
		if (everyTile || changed || _tileMemos[index].restless)
		{
			snapshotTile(index, tile, trackWalls);
		}
		else
		{
			snapshotTileLight(index, tile);
		}
		const TileView &tileView = _tileViews[index];
		noteLightsAndDevices(tile, tileView);
		const int top = tileView.max[2] >= tileView.min[2] ? tileView.max[2] : 0;
		_tileFlags[index] = (Uint8)((tileView.empty ? 0 : TileTraced) | (top << TileTopShift));
		_tileLights[index] = { tileView.light, tileView.ufoGlow, tileView.discovered, false };
	}
	// Which tiles have a roof over them (see indoorsAt): a floor or an object on any level above,
	// as the game counts a roof when it takes the sun's light away (TileEngine::calculateSunShading).
	// Each column from the top down.
	const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
	for (int column = 0; column < tilesPerLevel; ++column)
	{
		bool roofAbove = false;
		for (int index = column + tileCount - tilesPerLevel; index >= 0; index -= tilesPerLevel)
		{
			_tileLights[index].roofed = roofAbove;
			const Tile *tile = _save->getTile(index);
			roofAbove = roofAbove || tile->getMapData(O_FLOOR) || tile->getMapData(O_OBJECT);
		}
	}

	// Then keep only the nearest lamps, and put the swinging doors into the snapshot (their
	// tiles are marked in the flags too).
	keepNearestLights();
}

/**
 * Uses another view's voxel models (and frames) instead of building its own: the soldier card's
 * small view borrows the big view's. Whatever this view kept that points at the models it used
 * before (the ground beyond the map, doors swinging, what each tile showed last frame) is
 * forgotten, to be found again among the new owner's: the old owner may be about to go (a new
 * big view replaces it when the window changes size).
 * @param owner The view whose models to use.
 */
void FirstPersonView::shareCache(FirstPersonView *owner)
{
	_sharedModelsOwner = owner;
	_groundModel = nullptr;
	_waterModel = nullptr;
	_groundChosen = false;
	_doorAnimations.clear();
	gpuForgetModels();
	_solidMasks.clear();
	_paintedFloors.clear();
	_anyGlass = false;
	_wallPaints.clear(); // (found again with the ground, among the new owner's models)
	_wallPaintOf.clear();
	for (auto &memo : _tileMemos) memo = TileMemo{ {}, nullptr, {}, nullptr };
	for (auto &memos : _partMemos) memos = std::array<PartMemo, 4>();
}

/**
 * The solid mask of a tile with these four parts: for each row of voxels (by height and y), a
 * bit for each x where any of the parts has a voxel (window glass included: the ray notes glass
 * in the parts themselves). Made the first time the four parts come together on a tile, and
 * kept (the parts' models live as long as the view's model cache).
 * @param parts The tile's four parts (floor, west wall, north wall, object), null where none.
 * @return The mask, [z][y] with bit x.
 */
FirstPersonView::SolidRows FirstPersonView::solidMaskOf(const std::array<const VoxelModel*, 4> &parts)
{
	auto made = _solidMasks.find(parts);
	if (made == _solidMasks.end())
	{
		SolidMask mask = {};
		for (const VoxelModel *part : parts)
		{
			// The part, and its own ghost (VoxelModel::ghost).
			for (const VoxelModel *model : { part, part ? part->ghost : nullptr })
			{
				if (!model) continue;
				for (int z = 0; z < 24; ++z)
					for (int y = 0; y < 16; ++y)
						for (int x = 0; x < 16; ++x)
							if (model->color[z][y][x]) mask.rows[z][y] |= (Uint16)(1 << x);
			}
		}
		made = _solidMasks.emplace(parts, mask).first;
	}
	return made->second.rows;
}

/**
 * Notices what changed on a tile since the last frame: doors that just opened start swinging,
 * a fire that burnt out leaves the ground blackened, and a part destroyed since (not a door
 * opening) throws debris if the camera is near enough (480 voxels, 30 tiles) to see it.
 * @param index The tile's index in the map.
 * @param tile The tile.
 * @return True if one of its parts is another than last frame (see snapshotTiles).
 */
bool FirstPersonView::noticeTileChanges(int index, Tile *tile)
{
	trackDoors(index, tile);
	bool changed = false;

	// A fire burnt out here: the ground is left blackened (a scorch mark of radius 10 voxels on
	// the floor's surface in the middle of the tile; at most 200 of them).
	const bool burning = shownFire(index, tile) > 0;
	if (_burningLastFrame[index] && !burning && _scorches.size() < 200)
	{
		const Position tilePos = tile->getPosition();
		_scorches.push_back({ { tilePos.x * 16.0f + 8.0f, tilePos.y * 16.0f + 8.0f, tilePos.z * 24.0f + GroundTop - tile->getTerrainLevel() },
			10.0f });
	}
	_burningLastFrame[index] = burning;

	// Something destroyed here since last frame (not a door opening): debris. A part counts as
	// destroyed when it vanished or turned into its destroyed version (the record's "die MCD").
	// (A chain of explosions' wreckage, as its ripple passes over it: see shownPart.)
	for (int part = 0; part < 4; ++part)
	{
		const MapData *partBefore = _partsLastFrame[part][index], *partNow = shownPart(index, tile, part);
		_partsLastFrame[part][index] = partNow;
		changed = changed || partBefore != partNow;
		if (!partBefore || partBefore == partNow || partBefore->isDoor() || partBefore->isUFODoor() || !partBefore->getDataset()) continue;
		const MapData *destroyedVersion = partBefore->getDieMCD() ? partBefore->getDataset()->getObject(partBefore->getDieMCD()) : nullptr;
		if (partNow && partNow != destroyedVersion) continue;
		// dx, dy: from the camera to the middle of the tile, in voxels.
		const Position tilePos = tile->getPosition();
		const double dx = tilePos.x * 16.0 + 8.0 - _eye[0], dy = tilePos.y * 16.0 + 8.0 - _eye[1];
		if (dx * dx + dy * dy < 480.0 * 480.0) spawnDebris(tilePos, part, partBefore);
		// A pane of glass among it (on the game's own voxels, see paneOfGlass; or filled into its
		// openings, still whole): its shards fly too, and it's heard (the owner: "same shards and
		// sound").
		if (spawnGlassShards(tilePos, getModel(partBefore), !glassBroken(index, part), true, nullptr, nullptr, 1.2f))
		{
			Log(LOG_VERBOSE) << "OpenXcom3D: a pane of glass destroyed at " << tilePos.x << "," << tilePos.y << "," << tilePos.z << ", part " << part; // (for the tests)
		}
	}
	return changed;
}

/**
 * Starts watching a tile, on the first frame of a battle (or after a reload): notes its walls,
 * parts and fire, so the next frames can tell what changed (see noticeTileChanges).
 * @param index The tile's index in the map.
 * @param tile The tile.
 */
void FirstPersonView::startWatchingTile(int index, Tile *tile)
{
	_wallsLastFrame[0][index] = tile->getMapData(O_WESTWALL);
	_wallsLastFrame[1][index] = tile->getMapData(O_NORTHWALL);
	for (int part = 0; part < 4; ++part) _partsLastFrame[part][index] = shownPart(index, tile, part);
	_burningLastFrame[index] = shownFire(index, tile) > 0;
}

/**
 * A tile's part as the view shows it this frame: the game's, but while a chain of explosions
 * settled at once (setting "Instant chain explosions") hasn't rippled over the tile yet, the part
 * as it was before the chain (its wreckage comes as the fireball nearest to it grows over it, see
 * FirstPersonChains.cpp).
 * @param index The tile's index in the map.
 * @param tile The tile.
 * @param part The part (floor, west wall, north wall, object).
 * @return The part shown, or null for none.
 */
const MapData *FirstPersonView::shownPart(int index, Tile *tile, int part) const
{
	const FirstPerson::ChainHeldTile *held = _chainHeld.at(index);
	return held ? held->parts[part] : tile->getMapData((TilePart)part);
}

/**
 * A tile's fire as the view shows it this frame: the game's, but as it was before a chain of
 * explosions while the chain's ripple hasn't passed over the tile yet (see shownPart).
 * @param index The tile's index in the map.
 * @param tile The tile.
 * @return The fire shown (the game's number of turns it burns; 0 none).
 */
int FirstPersonView::shownFire(int index, const Tile *tile) const
{
	const FirstPerson::ChainHeldTile *held = _chainHeld.at(index);
	return held ? held->fire : tile->getFire();
}

/**
 * Takes one tile's snapshot for the rays: the voxel model (in the animation frame the tile
 * shows) in each of its parts, the frame each part dissolves into next, the frames of sliding
 * UFO doors, the bounding box of all its parts' solid voxels, which parts are light sources,
 * its light, and what of it has been seen (the fog of war; all of it in debug mode).
 * This runs for every tile every frame, but tiles hardly ever change: what each part showed
 * last frame is kept (see PartMemo), and its models are looked up again only when its record,
 * its frame, its door cut-out or its steady UFO model changed.
 * @param index The tile's index in the map.
 * @param tile The tile.
 * @param trackWalls False on the first frame of a battle: a UFO door already open then is
 *        shown open at once instead of sliding.
 */
void FirstPersonView::snapshotTile(int index, Tile *tile, bool trackWalls)
{
	// Start empty: no parts, no door swings (updateDoorSwings adds them later), nothing that
	// shines, and an inverted bounding box.
	TileView &tileView = _tileViews[index];
	tileView.empty = true;
	tileView.swing[0] = tileView.swing[1] = -1;
	tileView.emissive = 0;
	// Does the tile show anything that changes without its parts changing (see snapshotTiles)?
	bool restless = index == _hiddenFloor;
	for (int axis = 0; axis < 3; ++axis)
	{
		tileView.min[axis] = 255;
		tileView.max[axis] = 0;
	}

	// The four parts: floor, west wall, north wall, object.
	for (int part = 0; part < 4; ++part)
	{
		tileView.part[part] = nullptr;
		tileView.next[part] = nullptr;
		// Nothing there, or the floor left out (the lift platform the camera rides through), or a
		// part the eye is inside (Doom IDCLIP). (A chain of explosions' ripple not over it yet: as it
		// was, see shownPart.)
		const MapData *mapData = shownPart(index, tile, part);
		if (!mapData || (part == O_FLOOR && (int)index == _hiddenFloor)
			|| (!_leftOutParts.empty() && std::binary_search(_leftOutParts.begin(), _leftOutParts.end(), std::make_pair((int)index, part))))
		{
			continue;
		}

		// The part's memo: a new record (destroyed, swapped for its open door...) starts it afresh.
		PartMemo &memo = _partMemos[index][part];
		if (memo.mapData != mapData)
		{
			memo = PartMemo();
			memo.mapData = mapData;
			memo.frames = &getFrames(mapData);
			// A light source (by the game's rules) with bright voxels: those shine. Judged by its
			// first frame, so an animated lamp doesn't switch its shine on and off with its frames.
			memo.lamp = mapData->getLightSource() > 0 && getModel(mapData)->glowCount;
		}

		// Which animation frame does the part show? A UFO door's frames are its sliding open,
		// so it shows the game's frame: the game keeps the shown frame's sprite on the tile, so
		// find whose pixels those are. Anything else that animates (water, lights, computers,
		// the lit panels of UFOs) follows the view's calm clock instead (see CalmFrameTime).
		const Frames &frames = *memo.frames;
		restless = restless || frames.animated || mapData->isUFODoor(); // (its frames turn; a UFO door slides)
		int frame = 0;
		if (frames.animated && !mapData->isUFODoor())
		{
			frame = _calmFrame;
		}
		else if (frames.animated)
		{
			const Uint8 *shownBuffer = tile->getSprite((TilePart)part).getBuffer();
			for (int candidate = 0; candidate < 8; ++candidate)
			{
				if (frames.buffer[candidate] == shownBuffer)
				{
					frame = candidate;
					break;
				}
			}
		}

		// A UFO door opening shows its sliding frames (1 to 7), cut out where the door has gone,
		// at the view's own pace (UfoDoorTime) so it's seen sliding open. A closed UFO door
		// forgets when it last opened.
		const bool ufoWall = mapData->isUFODoor() && (part == O_WESTWALL || part == O_NORTHWALL);
		const bool cutout = ufoWall && tile->isUfoDoorOpen((TilePart)part);
		if (cutout)
		{
			auto opening = _ufoDoorOpening.find(index * 4 + part);
			if (opening == _ufoDoorOpening.end())
			{
				// It starts sliding now; already open when the view first saw it: no replay
				// (it started long enough ago to be fully open).
				opening = _ufoDoorOpening.emplace(index * 4 + part, trackWalls ? SDL_GetTicks() : SDL_GetTicks() - 10 * UfoDoorTime).first;
			}
			const Uint32 age = SDL_GetTicks() - opening->second; // ms since it started sliding
			// The frames of its sliding open: 1 to 7, or fewer where the record's list starts over (the
			// Lightning's door has four pictures, listed twice: shut, three of opening, shut again, the
			// three again. The map runs through all eight in a blink; here, at the view's pace, the door
			// was seen to open, shut and open again: the owner, "as the door would open twice in 3d and
			// skips a bit"). So only as far as each frame is a new picture, at the same pace a frame,
			// and then the game's last frame, the door open.
			int slidingFrames = 1;
			while (slidingFrames < 7)
			{
				bool seen = false;
				for (int earlier = 0; earlier <= slidingFrames; ++earlier) seen = seen || frames.sprite[earlier] == frames.sprite[slidingFrames + 1];
				if (seen) break;
				++slidingFrames;
			}
			const Uint32 slidingTime = UfoDoorTime * slidingFrames / 7;
			frame = age < slidingTime ? std::clamp(1 + (int)(age * 6 / UfoDoorTime), 1, slidingFrames) : 7;
		}
		else if (ufoWall)
		{
			_ufoDoorOpening.erase(index * 4 + part);
		}

		// The part's model in that frame (an empty one is left out), and the frame it steps to
		// next, to dissolve into over the step (not for UFO doors, nor for a steady UFO model): the
		// memo's, unless the frame, the cut-out or the steady model changed. A UFO part shows its
		// steady model instead with the UFO lights option: no frames, only its lights glow.
		const VoxelModel *steady = _ufoLightsOn ? frames.steady : nullptr;
		if (memo.frame != frame || memo.cutout != cutout || memo.steady != steady)
		{
			memo.frame = frame;
			memo.cutout = cutout;
			memo.steady = steady;
			memo.model = steady ? steady : getModel(mapData, frames.sprite[frame], cutout);
			// A tree's crown (foliage on a tile with no floor under it: a big tree's upper levels):
			// leafy (see leafyModel), also where a mod counts its forest among a UFO's tile sets
			// (X-Com Files: a crown has no lights for a steady model to show).
			if (!cutout && part == O_OBJECT && !tile->getMapData(O_FLOOR) && getModel(mapData, frames.sprite[frame], false)->foliage)
			{
				memo.model = leafyModel(mapData, frames.sprite[frame]);
			}
			memo.next = nullptr;
			if (frames.animated && !mapData->isUFODoor() && memo.model != frames.steady && frames.sprite[(frame + 1) % 8] != frames.sprite[frame])
			{
				const VoxelModel *nextModel = getModel(mapData, frames.sprite[(frame + 1) % 8], false);
				if (!nextModel->empty) memo.next = nextModel;
			}
		}
		// The glass in a window's opening broken (see breakGlassAt): the opening shown empty.
		const VoxelModel *model = memo.model, *next = memo.next;
		// A sliding door open, its sliding done: what is left of it a ghost (setting "Open doors as ghosts", see openDoorModel).
		if (Options::fpsGhostOpenDoors && cutout && frame == 7 && !model->empty && !model->ghostly)
		{
			model = openDoorModel(model);
		}
		// (the skin of the solid block the eye is inside, Doom IDCLIP: a ghost, see setGhostedParts)
		if (!_ghostedParts.empty() && !model->empty && !model->ghostly && std::binary_search(_ghostedParts.begin(), _ghostedParts.end(), std::make_pair((int)index, part)))
		{
			model = openDoorModel(model);
			next = nullptr;
		}
		restless = restless || model->openingGlass; // (its glass may be broken any frame)
		if (model->openingGlass && glassBroken(index, part))
		{
			model = openedModel(model);
			if (next && next->openingGlass) next = openedModel(next);
		}
		if (model->empty)
		{
			continue;
		}
		tileView.part[part] = model;
		tileView.next[part] = next;
		tileView.empty = false;

		// A light source with bright voxels shines there (see the memo).
		if (memo.lamp)
		{
			tileView.emissive |= (Uint8)(1 << part);
		}
		// A steady UFO model with lights: those shine too (see buildUfoModel).
		if (model->pulseKind)
		{
			tileView.emissive |= (Uint8)(1 << part);
		}

		// Grow the tile's bounding box by the part's (and by its own ghost's).
		for (const VoxelModel *grown : { model, model->ghost })
		{
			if (!grown) continue;
			for (int axis = 0; axis < 3; ++axis)
			{
				tileView.min[axis] = std::min(tileView.min[axis], grown->min[axis]);
				tileView.max[axis] = std::max(tileView.max[axis], grown->max[axis]);
			}
		}
	}

	// The floor under a UFO's hull, with the UFO's floor going on inside (see paintedFloor):
	// looked up only when the tile's parts changed since the last frame.
	TileMemo &memo = _tileMemos[index];
	if (tileView.part[O_FLOOR] && (tileView.part[O_WESTWALL] || tileView.part[O_NORTHWALL] || tileView.part[O_OBJECT]))
	{
		const std::array<const VoxelModel*, 4> shown = { tileView.part[0], tileView.part[1], tileView.part[2], tileView.part[3] };
		if (memo.paintParts != shown)
		{
			memo.paintParts = shown;
			memo.paintedFloor = paintedFloor(index, shown);
		}
		if (memo.paintedFloor)
		{
			tileView.part[O_FLOOR] = memo.paintedFloor;
			tileView.next[O_FLOOR] = nullptr; // (no dissolving into an unpainted next frame)
		}
	}

	// The parts' voxels together, for the rays (see traceTile): looked up only when the tile's
	// parts changed since the last frame.
	const std::array<const VoxelModel*, 4> parts = { tileView.part[0], tileView.part[1], tileView.part[2], tileView.part[3] };
	if (!memo.solid || memo.parts != parts)
	{
		memo.parts = parts;
		memo.solid = solidMaskOf(parts);
	}
	tileView.solid = memo.solid;
	memo.restless = restless;
	snapshotTileLight(index, tile);
}

/**
 * The part of a tile's snapshot read afresh every frame, whatever else of it is kept (see
 * snapshotTiles): its light, the fog of war, its smoke and whether it burns.
 * @param index The tile's index in the map.
 * @param tile The tile.
 */
void FirstPersonView::snapshotTileLight(int index, Tile *tile)
{
	const bool debug = _save->getDebugMode();
	TileView &tileView = _tileViews[index];
	// The game's shade for the tile as the map shows it (with night vision no darker than
	// NightVisionShade, and the map's debug vision: see mapShade), and the fog of war: the floor
	// and the object are seen with the floor, each wall on its own (a wall is seen from either
	// side, as on the isometric map).
	tileView.light = (Uint8)mapShade(tile->getShade());
	// A UFO's light (the UFO lights option), once that UFO has been seen (see findUfos).
	tileView.ufoGlow = _ufoLightsOn && _ufoOfTile[index] && _ufos[_ufoOfTile[index] - 1].seen ? _ufoGlowOfTile[index] : 0;
	tileView.discovered = debug || tile->isDiscovered(O_FLOOR);
	tileView.seen = (Uint8)((tileView.discovered ? (1 << O_FLOOR) | (1 << O_OBJECT) : 0)
		| (debug || tile->isDiscovered(O_WESTWALL) ? 1 << O_WESTWALL : 0)
		| (debug || tile->isDiscovered(O_NORTHWALL) ? 1 << O_NORTHWALL : 0));
	// Its smoke, once the tile has been seen (as the map shows smoke only there), and whether
	// it burns.
	tileView.smoke = tileView.discovered ? (Uint8)std::clamp(tile->getSmoke(), 0, 255) : 0;
	tileView.burning = shownFire(index, tile) > 0;
}

/**
 * The floor of a tile under a UFO's hull: the UFO's map leaves the floor of the
 * tiles it doesn't cover whole, so where a diagonal piece of its hull stands, the tile's floor is
 * still the ground it landed on, and the 3D view showed grass (or road...) inside the UFO. (The
 * map draws the inside of the hull over it in the hull's art: black.) So the UFO's floor goes on
 * inside: from the tile's sides that border the UFO's floor (a neighbour on the same level whose
 * floor is of a data set UFOs are built from, the hull's or its fittings'), the floor is flooded
 * up to the hull's voxels standing on it, and what's reached takes the colours of those
 * neighbours' floors at the same spot (see below); the rest (outside the hull) stays the ground.
 * Made once for each set of parts and neighbouring floors.
 * @param index The tile's index.
 * @param parts The tile's four parts' models, as shown: the floor first.
 * @return The floor with the UFO's floor going on inside, or null if it isn't such a tile.
 */
const FirstPersonView::VoxelModel *FirstPersonView::paintedFloor(int index, const std::array<const VoxelModel*, 4> &parts)
{
	// The floor's data set, and the walls and object of a UFO's set standing on it (the hull;
	// other buildings, a barn's walls on a field, keep their ground as it is).
	const Tile *tile = _save->getTile(index);
	const MapData *floorData = tile->getMapData(O_FLOOR);
	if (!floorData || !floorData->getDataset())
	{
		return nullptr;
	}
	const MapDataSet *hullSet = nullptr;
	for (int part = O_WESTWALL; part <= O_OBJECT; ++part)
	{
		const MapData *partData = tile->getMapData((TilePart)part);
		if (parts[part] && partData && partData->getDataset() && partData->getDataset() != floorData->getDataset()
			&& _ufoDataSets.count(partData->getDataset()->getName()))
		{
			hullSet = partData->getDataset();
			break;
		}
	}
	if (!hullSet)
	{
		return nullptr;
	}

	// The sides bordering the UFO's floor (west, north, east, south): each one's floor, or null.
	const Position here = tile->getPosition();
	const int sideSteps[4][2] = { { -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 } };
	std::array<const VoxelModel*, 4> sideFloors = {};
	bool bordering = false;
	for (int side = 0; side < 4; ++side)
	{
		const Tile *neighbour = _save->getTile(here + Position(sideSteps[side][0], sideSteps[side][1], 0));
		const MapData *neighbourFloor = neighbour ? neighbour->getMapData(O_FLOOR) : nullptr;
		if (neighbourFloor && neighbourFloor->getDataset() && neighbourFloor->getDataset() != floorData->getDataset()
			&& _ufoDataSets.count(neighbourFloor->getDataset()->getName()))
		{
			const VoxelModel *model = getModel(neighbourFloor);
			if (model->empty || model->ghostly) continue;
			sideFloors[side] = model;
			bordering = true;
		}
	}
	if (!bordering)
	{
		return nullptr;
	}

	// Made already for these parts and those floors?
	const std::array<const VoxelModel*, 8> key = { parts[0], parts[1], parts[2], parts[3], sideFloors[0], sideFloors[1], sideFloors[2], sideFloors[3] };
	auto made = _paintedFloors.find(key);
	if (made != _paintedFloors.end())
	{
		return made->second.get();
	}

	// Each column's top floor voxel (-1: none), and whether the hull stands there (a voxel of
	// the walls or the object within six voxels above the floor).
	const VoxelModel &floor = *parts[O_FLOOR];
	int top[16][16];
	bool standing[16][16];
	for (int y = 0; y < 16; ++y)
	{
		for (int x = 0; x < 16; ++x)
		{
			top[y][x] = -1;
			for (int z = floor.max[2]; z >= floor.min[2] && top[y][x] < 0; --z)
			{
				if (floor.color[z][y][x]) top[y][x] = z;
			}
			standing[y][x] = false;
			const int fromZ = std::max(0, top[y][x] + 1), toZ = std::min(23, top[y][x] + 6);
			for (int part = O_WESTWALL; part <= O_OBJECT && !standing[y][x]; ++part)
			{
				if (!parts[part] || parts[part]->ghostly) continue;
				for (int z = fromZ; z <= toZ && !standing[y][x]; ++z) standing[y][x] = parts[part]->color[z][y][x] != 0;
			}
		}
	}

	// Flood from the bordering sides, side by side, up to where the hull stands.
	bool inside[16][16] = {};
	std::vector<std::pair<int, int>> toVisit;
	auto visit = [&](int x, int y)
	{
		if (x < 0 || y < 0 || x > 15 || y > 15 || inside[y][x] || standing[y][x]) return;
		inside[y][x] = true;
		toVisit.push_back({ x, y });
	};
	for (int along = 0; along < 16; ++along)
	{
		if (sideFloors[0]) visit(0, along);
		if (sideFloors[1]) visit(along, 0);
		if (sideFloors[2]) visit(15, along);
		if (sideFloors[3]) visit(along, 15);
	}
	while (!toVisit.empty())
	{
		const auto [x, y] = toVisit.back();
		toVisit.pop_back();
		visit(x - 1, y);
		visit(x + 1, y);
		visit(x, y - 1);
		visit(x, y + 1);
	}

	// The floors to take colours from, the fullest first (by how many of its columns have a
	// voxel): a neighbour that is a hull tile itself may have only part of a floor (a triangle by
	// its own diagonal piece of hull), and the colours of that one alone left most of this floor
	// the ground's (grass in the Abductor's north-west corners).
	std::vector<std::pair<int, const VoxelModel*>> sources; // (columns with a voxel, floor)
	for (const VoxelModel *sideFloor : sideFloors)
	{
		if (!sideFloor || std::any_of(sources.begin(), sources.end(), [sideFloor](const auto &source) { return source.second == sideFloor; }))
		{
			continue;
		}
		int columns = 0;
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x) columns += sideFloor->plan[y][x] != 0;
		sources.push_back({ columns, sideFloor });
	}
	std::stable_sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	// A spot's colour: as seen from above on the first of those floors with a voxel there, or
	// failing that, at the fullest one's nearest spot that has one (rings round the spot).
	const VoxelModel &fullest = *sources.front().second;
	auto colorAt = [&](int x, int y) -> Uint8
	{
		for (const auto &source : sources)
		{
			if (source.second->plan[y][x]) return source.second->plan[y][x];
		}
		for (int reach = 1; reach < 16; ++reach)
		{
			for (int dy = -reach; dy <= reach; ++dy)
			{
				for (int dx = -reach; dx <= reach; ++dx)
				{
					const int nearX = x + dx, nearY = y + dy;
					if (std::max(std::abs(dx), std::abs(dy)) == reach && nearX >= 0 && nearY >= 0 && nearX < 16 && nearY < 16 && fullest.plan[nearY][nearX])
					{
						return fullest.plan[nearY][nearX];
					}
				}
			}
		}
		return 0;
	};

	// What's inside takes those floors' colours at the same spot (on this floor's top voxel).
	std::unique_ptr<VoxelModel> painted;
	for (int y = 0; y < 16; ++y)
	{
		for (int x = 0; x < 16; ++x)
		{
			if (!inside[y][x] || top[y][x] < 0) continue;
			const Uint8 color = colorAt(x, y);
			if (!color) continue;
			if (!painted) painted = std::make_unique<VoxelModel>(floor);
			painted->color[top[y][x]][y][x] = color;
		}
	}
	return (_paintedFloors[key] = std::move(painted)).get();
}

/**
 * Notes the light sources and UFO machinery on a tile near the camera: lamps (within 480
 * voxels, 30 tiles) for the pools of light they cast, and discovered UFO power sources and
 * navigation consoles (within 240 voxels) for their particles. A lamp shines on what you see
 * even if its own tile is still unexplored.
 * @param tile The tile.
 * @param tileView Its snapshot for this frame (see snapshotTile).
 */
void FirstPersonView::noteLightsAndDevices(const Tile *tile, const TileView &tileView)
{
	if (tileView.empty)
	{
		return;
	}
	// The tile's north-west bottom corner in world voxels, and (dx, dy) from the camera to the
	// middle of the tile.
	const Position tilePos = tile->getPosition();
	const float originX = tilePos.x * 16.0f, originY = tilePos.y * 16.0f, originZ = tilePos.z * 24.0f;
	const float dx = originX + 8.0f - (float)_eye[0], dy = originY + 8.0f - (float)_eye[1];
	if (dx * dx + dy * dy < 480.0f * 480.0f)
	{
		for (int part = 0; part < 4; ++part)
		{
			const VoxelModel *model = tileView.part[part];
			const MapData *mapData = tile->getMapData((TilePart)part);
			if (!model || !mapData) continue;

			// A part that shines by the game's rules: a lamp at the centre of its bright voxels,
			// as strong as the game's light level for it says (12 or more is full strength), in
			// its glow colour. Taken from its first frame, so the pool of light it casts stays
			// put while the part animates.
			if (((tileView.emissive >> part) & 1) && mapData->getLightSource() > 0)
			{
				const VoxelModel *steadyModel = getModel(mapData);
				LightSpot light = { { originX + steadyModel->glow[0], originY + steadyModel->glow[1], originZ + steadyModel->glow[2] },
					std::min(1.0f, mapData->getLightSource() / 12.0f), steadyModel->glowColor };
				_nearbyLights.push_back(light);
			}

			// A UFO power source or navigation console that has been seen, closer: the box of
			// its solid voxels (centre and half size, in world voxels).
			const SpecialTileType specialType = mapData->getSpecialType();
			if (tileView.discovered && (specialType == UFO_POWER_SOURCE || specialType == UFO_NAVIGATION) && dx * dx + dy * dy < 240.0f * 240.0f)
			{
				DeviceSpot device;
				for (int axis = 0; axis < 3; ++axis)
				{
					const float axisOrigin = axis == 0 ? originX : axis == 1 ? originY : originZ;
					device.pos[axis] = axisOrigin + (model->min[axis] + model->max[axis] + 1) * 0.5f;
					device.half[axis] = (model->max[axis] + 1 - model->min[axis]) * 0.5f;
				}
				device.power = specialType == UFO_POWER_SOURCE;
				_nearbyDevices.push_back(device);
			}
		}
	}
}

/**
 * Keeps only the 16 lamps nearest the camera (measured across the ground, ignoring height).
 * Every shaded pixel looks at every lamp, so their number is what the lighting costs.
 */
void FirstPersonView::keepNearestLights()
{
	const size_t maxLights = 16;
	if (_nearbyLights.size() > maxLights)
	{
		// Sort just the nearest 16 to the front (by squared distance), and drop the rest.
		const float eyeX = (float)_eye[0], eyeY = (float)_eye[1];
		std::partial_sort(_nearbyLights.begin(), _nearbyLights.begin() + maxLights, _nearbyLights.end(),
			[eyeX, eyeY](const LightSpot &first, const LightSpot &second) {
			return (first.pos[0] - eyeX) * (first.pos[0] - eyeX) + (first.pos[1] - eyeY) * (first.pos[1] - eyeY)
				< (second.pos[0] - eyeX) * (second.pos[0] - eyeX) + (second.pos[1] - eyeY) * (second.pos[1] - eyeY);
		});
		_nearbyLights.resize(maxLights);
	}
}

/**
 * Puts the doors swinging into this frame's snapshot. Each door turns as a slab around its hinge
 * over DoorSwingTime ms (eased in and out), coloured from the door as it was, from the wall it
 * left to where the game has it now (see trackDoors); meanwhile the door's new part is hidden,
 * so the game's own door appears only when the swing ends, right where the swing put it.
 * @param tileCount The number of tiles on the map.
 */
void FirstPersonView::updateDoorSwings(int tileCount)
{
	_doorSwings.clear();
	const Uint32 now = SDL_GetTicks();
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY();
	// Points a tile's rays at a swing, in its first free slot: two doors can swing through one
	// tile at once (its west and north doors). A third at the same moment isn't traced.
	auto addSwingTo = [this](int tileIndex, Sint16 swingIndex)
	{
		TileView &tileView = _tileViews[tileIndex];
		for (Sint16 &slot : tileView.swing)
		{
			if (slot < 0) { slot = swingIndex; break; }
		}
		tileView.empty = false;
		_tileFlags[tileIndex] |= TileTraced | TileSwinging; // the rays look into it, and always trace the door
		_swungTiles.push_back(tileIndex); // (its snapshot is taken afresh next frame, see snapshotTiles)
	};
	for (auto door = _doorAnimations.begin(); door != _doorAnimations.end();)
	{
		// How far the swing has got (0 to 1), and where the door's tile is (in tiles).
		const float progress = std::min(1.0f, (now - door->start) / (float)DoorSwingTime);
		const int tileX = door->tile % mapSizeX, tileY = (door->tile / mapSizeX) % mapSizeY, tileZ = door->tile / (mapSizeX * mapSizeY);
		const bool westWall = door->part == O_WESTWALL;

		// Forget the swing when it has ended (the game's own door shows where it came to rest),
		// or when the door is no longer there (it turned again, or was destroyed: its record has
		// gone from the part). (The swing index has to fit a Sint16.)
		const bool stillThere = door->tile < tileCount && _save->getTile(door->tile)->getMapData((TilePart)door->hiddenPart) == door->openDoor;
		if (!stillThere || progress >= 1.0f || _doorSwings.size() >= 32000)
		{
			door = _doorAnimations.erase(door);
			continue;
		}

		// The door's angle around the hinge, eased (smoothstep), in radians from +x (east)
		// towards +y (south).
		const float eased = progress * progress * (3.0f - 2.0f * progress);
		const float angle = door->fromAngle + (door->toAngle - door->fromAngle) * eased;

		// The swinging slab: hinged one voxel in from the tile's north-west corner, as tall as
		// the door, in world voxels.
		DoorSwing swing;
		swing.hinge[0] = tileX * 16.0f + 1.0f;
		swing.hinge[1] = tileY * 16.0f + 1.0f;
		swing.dir[0] = std::cos(angle);
		swing.dir[1] = std::sin(angle);
		swing.z0 = tileZ * 24.0f + door->model->min[2];
		swing.z1 = tileZ * 24.0f + door->model->max[2] + 1;
		swing.model = door->model;
		swing.westWall = westWall;

		// In the door's own tile (it turns inside it): hide the door's new part and point the
		// rays at the swing.
		TileView &tileView = _tileViews[door->tile];
		_swungTiles.push_back(door->tile);
		tileView.part[door->hiddenPart] = nullptr;
		tileView.next[door->hiddenPart] = nullptr;
		addSwingTo(door->tile, (Sint16)_doorSwings.size());
		_doorSwings.push_back(swing);
		++door;
	}
}

/// The length of (x, y). (Not std::hypot: on floats that is hypotf, which asks for glibc 2.35, and the release must run on 2.34.)
static float flatLength(float x, float y)
{
	return std::sqrt(x * x + y * y);
}

/**
 * Turns a frame's shape from its art, as on a lathe, from a height up: for a thing in motion round its own upright
 * axis that the mod's table names (`turned:`, see FirstPersonView::turnedFrom). The game's shape is one for all the
 * frames, so a collar rising round a rod and the thin rod standing alone were the same block with other paint (the
 * owner: the pedestals of XPiratez's T'leth base "do not move as shapes").
 * The thing is taken as discs about the axis, one a height. The art shows a disc as an ellipse: as wide as the disc
 * on the row its middle lands on, and its far rim on the rows above, narrowing. So, from the bottom up, a height
 * whose row is wider than the far rims of the discs below it would be has a disc of its own, as wide as the row
 * (a drum's side, a collar); a height whose row those rims account for shows nothing of its own, and what stands
 * there is what stands on the disc: the next disc found above, or, with none above, the game's shape. Read plainly,
 * row by row, a flat slab became a dome.
 * (A disc is cut by the tile's sides, so a wide one is the tile's square, and its far rim the square's corner.)
 * Voxels the game's shape hasn't got are coloured as the others, by the pixel they land on, or the nearest drawn one
 * toward the axis. Below the height the game's shape stands.
 * @param model The frame's model, built from the game's shape.
 * @param sprite The frame's art.
 * @param yOffset How much higher than usual the map draws the sprite (see buildModel).
 * @param from The height (voxels) the turned shape starts at.
 */
static void turnFromArt(FirstPersonView::VoxelModel &model, const Surface *sprite, int yOffset, int from)
{
	const int width = sprite->getWidth(), height = sprite->getHeight();
	// The shape's own upright axis (the middle of its voxels), and how far it reaches from it at each height.
	double sumX = 0.0, sumY = 0.0;
	int count = 0;
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
				if (model.color[z][y][x]) { sumX += x; sumY += y; ++count; }
	if (!count) return;
	const float axisX = (float)(sumX / count), axisY = (float)(sumY / count);
	float shape[Position::TileZ] = {}, art[Position::TileZ] = {}, reach[Position::TileZ] = {};
	bool own[Position::TileZ] = {};
	for (int z = 0; z < Position::TileZ; ++z)
	{
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
				if (model.color[z][y][x]) shape[z] = std::max(shape[z], flatLength(x - axisX, y - axisY) + 0.5f);
		// (Half the row's span: a pixel across is a voxel times the square root of two along the tile's diagonal.)
		const int row = (int)std::lround(24.0f + (axisX + axisY) * 0.5f - z + yOffset);
		int first = width, last = -1;
		for (int x = 0; row >= 0 && row < height && x < width; ++x)
		{
			if (sprite->getPixel(x, row))
			{
				first = std::min(first, x);
				last = x;
			}
		}
		art[z] = last < 0 ? 0.0f : (last - first + 1) * 0.5f / 1.41421356f;
	}
	// From the bottom up: the heights with a disc of their own.
	for (int z = 0; z < Position::TileZ; ++z)
	{
		// As wide as the far rims of the discs below are on this row. A disc's rim d rows up: the square root of its
		// reach squared less two d squared; and no disc goes past the tile, whose far corner narrows by the square
		// root of two a row (a slab filling the tile shows a diamond, not an ellipse).
		float rims = 0.0f;
		for (int below = 0; below < z; ++below)
		{
			if (!own[below]) continue;
			const float rows = (float)(z - below), left = reach[below] * reach[below] - 2.0f * rows * rows;
			const float rim = std::min(left > 0.0f ? std::sqrt(left) : 0.0f, (Position::TileXY * 0.5f - rows) * 1.41421356f);
			rims = std::max(rims, rim);
		}
		own[z] = art[z] > rims + TurnedRimSlack;
		reach[z] = own[z] ? art[z] : 0.0f;
	}
	// From the top down: the others have what stands on the disc below them, no wider than their row.
	float above = -1.0f;
	for (int z = Position::TileZ - 1; z >= 0; --z)
	{
		if (own[z]) above = reach[z];
		else reach[z] = std::min(art[z], above >= 0.0f ? above : shape[z]);
	}
	auto pixelAt = [&](int x, int y, int z) -> Uint8
	{
		const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
		return spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height ? sprite->getPixel(spriteX, spriteY) : 0;
	};
	for (int z = std::max(0, from); z < Position::TileZ; ++z)
	{
		for (int y = 0; y < Position::TileXY; ++y)
		{
			for (int x = 0; x < Position::TileXY; ++x)
			{
				const float fromAxisX = x - axisX, fromAxisY = y - axisY;
				const bool inside = reach[z] > 0.0f && fromAxisX * fromAxisX + fromAxisY * fromAxisY <= reach[z] * reach[z];
				if (!inside)
				{
					model.color[z][y][x] = 0;
					model.glass[z][y] &= (Uint16)~(1 << x);
					model.clear[z][y] &= (Uint16)~(1 << x);
				}
				else if (!model.color[z][y][x])
				{
					// (the pixel it lands on, or the first drawn one on the way to the axis)
					Uint8 color = 0;
					float atX = (float)x, atY = (float)y;
					for (int step = 0; !color && step < Position::TileXY; ++step)
					{
						color = pixelAt((int)std::lround(atX), (int)std::lround(atY), z);
						const float toAxisX = axisX - atX, toAxisY = axisY - atY, far = flatLength(toAxisX, toAxisY);
						if (far < 0.5f) break;
						atX += toAxisX / far;
						atY += toAxisY / far;
					}
					model.color[z][y][x] = color;
				}
			}
		}
	}
	// A voxel of a disc's top that the art hides behind what stands on the disc (the far side of a collar's top, behind
	// the rod) landed on that other's pixel: the far half of the top had the rod's shaded side for its colours. Turned
	// about its axis the thing is the same all round, so such a voxel takes the colour of one of the top's that the art
	// does show, as far from the axis: of those, the nearest to the point across the axis from it. (Only the tops: a
	// side's voxels hidden behind the thing's own side higher up land on that side, which is their look.)
	// (The art looks along x, y and z all growing by one.)
	struct Shown { float far; int x, y; Uint8 color; };
	std::vector<Shown> shown;
	std::vector<std::pair<int, int>> behind;
	for (int z = std::max(0, from); z < Position::TileZ; ++z)
	{
		shown.clear();
		behind.clear();
		for (int y = 0; y < Position::TileXY; ++y)
		{
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (!model.color[z][y][x] || (z + 1 < Position::TileZ && model.color[z + 1][y][x])) continue;
				bool hidden = false;
				for (int step = 1; !hidden && x + step < Position::TileXY && y + step < Position::TileXY && z + step < Position::TileZ; ++step)
				{
					hidden = model.color[z + step][y + step][x + step] != 0;
				}
				if (hidden) behind.emplace_back(x, y);
				else shown.push_back({ flatLength(x - axisX, y - axisY), x, y, model.color[z][y][x] });
			}
		}
		for (const auto &voxel : behind)
		{
			const float far = flatLength(voxel.first - axisX, voxel.second - axisY), acrossX = 2.0f * axisX - voxel.first, acrossY = 2.0f * axisY - voxel.second;
			float best = FLT_MAX;
			for (const Shown &one : shown)
			{
				// (as far from the axis first: a voxel off counts as four across)
				const float score = std::abs(one.far - far) * 4.0f + flatLength(one.x - acrossX, one.y - acrossY);
				if (score < best)
				{
					best = score;
					model.color[z][voxel.second][voxel.first] = one.color;
				}
			}
		}
	}
	// (Its box again, over what there is now.)
	bool any = false;
	for (int axis = 0; axis < 3; ++axis)
	{
		model.min[axis] = 255;
		model.max[axis] = 0;
	}
	for (int z = 0; z < Position::TileZ; ++z)
		for (int y = 0; y < Position::TileXY; ++y)
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (!model.color[z][y][x]) continue;
				any = true;
				const int voxel[3] = { x, y, z };
				for (int axis = 0; axis < 3; ++axis)
				{
					model.min[axis] = std::min<int>(model.min[axis], voxel[axis]);
					model.max[axis] = std::max<int>(model.max[axis], voxel[axis]);
				}
			}
	model.empty = !any;
}

/**
 * The height from which an animated part's frames are each turned from their art (see turnFromArt): what the mod's
 * table `turned:` gives for the part's data set and record, if its frames are different pictures. Kept for the record.
 * @param mapData The tile part's record.
 * @return The height in voxels, or -1 for a part the table doesn't name.
 */
int FirstPersonView::turnedFrom(const MapData *mapData)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->turnedFrom(mapData);
	}
	auto known = _turnedParts.find(mapData);
	if (known != _turnedParts.end())
	{
		return known->second;
	}
	int &from = _turnedParts[mapData];
	from = -1;
	MapDataSet *dataSet = mapData->getDataset();
	if (!dataSet || mapData->isUFODoor() || !artAnimates(mapData))
	{
		return from;
	}
	for (size_t record = 0; record < dataSet->getSize(); ++record)
	{
		if (dataSet->getObject(record) == mapData)
		{
			from = _game->getMod()->getFirstPerson().getTurnedFrom(dataSet->getName(), (int)record);
			break;
		}
	}
	return from;
}

/**
 * Tells whether a record's frames are different pictures: an animated part by its art, not only by its numbers (a
 * mod's wall may have two sprite numbers for one picture, see getModel).
 * @param mapData The tile part's record.
 * @return True if any frame's picture differs from the first's.
 */
bool FirstPersonView::artAnimates(const MapData *mapData)
{
	const Frames &frames = getFrames(mapData);
	SurfaceSet *art = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr;
	const Surface *first = art && frames.animated ? art->getFrame(frames.sprite[0]) : nullptr;
	for (int frame = 1; first && frame < 8; ++frame)
	{
		const Surface *other = frames.sprite[frame] != frames.sprite[0] ? art->getFrame(frames.sprite[frame]) : nullptr;
		if (!other) continue;
		if (other->getWidth() != first->getWidth() || other->getHeight() != first->getHeight()) return true;
		for (int y = 0; y < first->getHeight(); ++y)
			for (int x = 0; x < first->getWidth(); ++x)
				if (first->getPixel(x, y) != other->getPixel(x, y)) return true;
	}
	return false;
}

/**
 * Tells whether a record's frames give different models: whether its animation shows in the view by its voxels (or
 * its own ghost's).
 * @param mapData The tile part's record.
 * @return True if any frame's model differs from the first frame's.
 */
bool FirstPersonView::modelsAnimate(const MapData *mapData)
{
	const Frames &frames = getFrames(mapData);
	const VoxelModel *first = getModel(mapData, frames.sprite[0], false);
	for (int frame = 1; frames.animated && frame < 8; ++frame)
	{
		const VoxelModel *other = getModel(mapData, frames.sprite[frame], false);
		if (other == first) continue;
		if (std::memcmp(other->color, first->color, sizeof(first->color)) != 0) return true;
		if (other->ghost != first->ghost && (!other->ghost || !first->ghost || std::memcmp(other->ghost->color, first->ghost->color, sizeof(first->color)) != 0)) return true;
	}
	return false;
}

/**
 * The art of an animated part that its model has no voxels for, to be drawn as a sprite standing in its tile (the
 * owner: "Test all animated world tiles so they actually animate in 3D"). A model is the game's shape for the part
 * coloured from its art, and the art is often more than the shape: a torch's flame above its holder, a candelabra's
 * candles, a hologram turning over its base, an aerial rising from a mound. Where that part of the art is what
 * animates, the part stood still in the view though the map shows it alive: 73 of XPiratez's animated records, 26 of
 * The X-Com Files', 3 of TFTD's, 1 of UFO's (see checkAnimatedParts). And some parts without any voxels got no ghost
 * either, their art being nowhere their kind of ghost stands (bubbles, sea spray, a planet turning as a "floor"):
 * those were not drawn at all.
 *
 * With voxels, and only where no two of its frames' models differ (the part stands still in the view): the pixels
 * no voxel of the shape lands on, of the pieces of art that change from frame to frame
 * (a piece: drawn pixels touching each other, among those the shape leaves out; so a flame comes whole, with the
 * candle under it, and art that never changes stays as it was). They stand on the shape: the sprite's pixel where
 * the middle of the shape's top lands is held there, so the piece keeps its place over the part from every side.
 * Without voxels and without a ghost in any frame: the whole art, standing where its middle column meets the floor
 * across the tile. A flame (see partKind) is drawn by its own rule; a sliding door's frames are its opening.
 * @param mapData The tile part's record.
 * @return What to draw, or null for none.
 */
const FirstPersonView::ArtOverlay *FirstPersonView::artOverlay(const MapData *mapData)
{
	if (_sharedModelsOwner)
	{
		return _sharedModelsOwner->artOverlay(mapData);
	}
	auto known = _artOverlays.find(mapData);
	if (known != _artOverlays.end())
	{
		return known->second.frames[0] ? &known->second : nullptr;
	}
	ArtOverlay &overlay = _artOverlays[mapData];
	const Frames &frames = getFrames(mapData);
	SurfaceSet *art = mapData->getDataset() ? mapData->getDataset()->getSurfaceset() : nullptr;
	if (!art || mapData->isUFODoor() || !artAnimates(mapData))
	{
		return nullptr;
	}
	if (const VoxelModel *kind = partKind(mapData, false))
	{
		if (kind->ghostKind == 5) return nullptr; // (a flame: drawn as its sprite already)
	}
	const Surface *pictures[8];
	int width = 0, height = 0;
	for (int frame = 0; frame < 8; ++frame)
	{
		pictures[frame] = art->getFrame(frames.sprite[frame]);
		if (!pictures[frame] || (frame && (pictures[frame]->getWidth() != width || pictures[frame]->getHeight() != height))) return nullptr;
		width = pictures[frame]->getWidth();
		height = pictures[frame]->getHeight();
	}
	// The models of its frames: has it voxels (its own, or a ghost's) in any of them?
	const VoxelModel *models[8];
	bool anyVoxels = false, ownVoxels = false;
	for (int frame = 0; frame < 8; ++frame)
	{
		models[frame] = getModel(mapData, frames.sprite[frame], false);
		anyVoxels = anyVoxels || !models[frame]->empty || models[frame]->ghost;
		// (Its own voxels, or a ghost the view draws solid: low art laid or stood on the floor, which shows what of the
		// art is near the ground and nothing of what floats over it: a globe turning over its projector.)
		ownVoxels = ownVoxels || (!models[frame]->empty && (!models[frame]->ghostly || models[frame]->solidGhost));
	}
	const int yOffset = mapData->getYOffset();
	std::vector<char> shown[8];
	int total = 0;
	if (!anyVoxels)
	{
		// Nothing of it is drawn: its whole art, where its middle column meets the floor across the tile's middle.
		long columns = 0, drawn = 0;
		for (int frame = 0; frame < 8; ++frame)
		{
			shown[frame].assign((size_t)width * height, 0);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					if (pictures[frame]->getPixel(x, y)) { shown[frame][(size_t)y * width + x] = 1; columns += x; ++drawn; }
		}
		if (!drawn) return nullptr;
		total = (int)drawn;
		const float column = std::clamp(columns / (float)drawn, 0.0f, 32.0f);
		overlay.anchorX = (int)std::lround(column);
		overlay.anchorY = 32 + yOffset;
		overlay.point[0] = column / 2.0f;
		overlay.point[1] = 16.0f - column / 2.0f;
		overlay.point[2] = 0.0f;
	}
	else if (ownVoxels)
	{
		// The pixels the shape's voxels land on (the first frame's: the game's shape is one for all its frames),
		// the middle of the shape, and its top.
		const VoxelModel &shape = *models[0];
		if (shape.empty || (shape.ghostly && !shape.solidGhost)) return nullptr;
		// Only a part that stands still in the view: no two of its frames' models differ. One whose model shows its
		// animation is left as it is, whatever more its art has round it (a first version drew a sprite for every part
		// with changing art outside its shape: 697 of XPiratez's records, the sea's every tile among TFTD's).
		if (modelsAnimate(mapData)) return nullptr;
		std::vector<char> covered((size_t)width * height, 0);
		double sumX = 0.0, sumY = 0.0;
		int count = 0;
		for (int z = 0; z < Position::TileZ; ++z)
			for (int y = 0; y < Position::TileXY; ++y)
				for (int x = 0; x < Position::TileXY; ++x)
				{
					if (!shape.color[z][y][x]) continue;
					sumX += x; sumY += y; ++count;
					const int spriteX = 16 + x - y, spriteY = 24 + (x + y) / 2 - z + yOffset;
					if (spriteX >= 0 && spriteY >= 0 && spriteX < width && spriteY < height) covered[(size_t)spriteY * width + spriteX] = 1;
				}
		if (!count) return nullptr;
		// The pixels that change from frame to frame.
		std::vector<char> changing((size_t)width * height, 0);
		for (int frame = 1; frame < 8; ++frame)
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					if (pictures[frame]->getPixel(x, y) != pictures[0]->getPixel(x, y)) changing[(size_t)y * width + x] = 1;
		// Each frame: the pieces of its art outside the shape that hold a changing pixel, whole.
		std::vector<int> toVisit;
		int seeds = 0;
		for (int frame = 0; frame < 8; ++frame)
		{
			shown[frame].assign((size_t)width * height, 0);
			toVisit.clear();
			for (int at = 0; at < width * height; ++at)
			{
				if (changing[at] && !covered[at] && pictures[frame]->getPixel(at % width, at / width))
				{
					shown[frame][at] = 1;
					toVisit.push_back(at);
					++seeds;
				}
			}
			while (!toVisit.empty())
			{
				const int at = toVisit.back();
				toVisit.pop_back();
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int x = at % width + dx, y = at / width + dy;
						if (x < 0 || y < 0 || x >= width || y >= height) continue;
						const int next = y * width + x;
						if (shown[frame][next] || covered[next] || !pictures[frame]->getPixel(x, y)) continue;
						shown[frame][next] = 1;
						toVisit.push_back(next);
					}
			}
			for (const char pixel : shown[frame]) total += pixel;
		}
		if (seeds < ArtOverlayMinPixels) return nullptr; // (a speck or two: not worth a sprite)
		const float middleX = (float)(sumX / count) + 0.5f, middleY = (float)(sumY / count) + 0.5f, top = shape.max[2] + 1.0f;
		overlay.point[0] = middleX;
		overlay.point[1] = middleY;
		overlay.point[2] = top;
		overlay.anchorX = (int)std::lround(16.0f + middleX - middleY);
		overlay.anchorY = (int)std::lround(24.0f + (middleX + middleY) / 2.0f - top + yOffset);
	}
	else
	{
		return nullptr; // (a ghost shows it)
	}
	overlay.lit = mapData->getLightSource() > 0;
	for (int frame = 0; frame < 8; ++frame)
	{
		overlay.frames[frame] = new Surface(width, height);
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
				if (shown[frame][(size_t)y * width + x]) overlay.frames[frame]->setPixel(x, y, pictures[frame]->getPixel(x, y));
	}
	_anyArtOverlay = true;
	return &overlay;
}

void FirstPersonView::clearArtOverlays()
{
	for (auto &entry : _artOverlays)
	{
		for (Surface *&frame : entry.second.frames)
		{
			delete frame;
			frame = nullptr;
		}
	}
	_artOverlays.clear();
	_anyArtOverlay = false;
}

/**
 * Every tile set the mod knows: its terrains', its UFOs' and its crafts' (for the developer's checks below).
 * @param mod The mod.
 * @return The sets, each once.
 */
static std::vector<MapDataSet*> everyTileSet(Mod *mod)
{
	std::vector<MapDataSet*> sets;
	auto add = [&](RuleTerrain *terrain)
	{
		if (!terrain) return;
		for (MapDataSet *set : *terrain->getMapDataSets())
		{
			if (std::find(sets.begin(), sets.end(), set) == sets.end()) sets.push_back(set);
		}
	};
	for (const std::string &name : mod->getTerrainList()) add(mod->getTerrain(name));
	for (const std::string &name : mod->getUfosList()) add(mod->getUfo(name) ? mod->getUfo(name)->getBattlescapeTerrainData() : nullptr);
	for (const std::string &name : mod->getCraftsList()) add(mod->getCraft(name) ? mod->getCraft(name)->getBattlescapeTerrainData() : nullptr);
	return sets;
}

/**
 * A developer's check, run where a battle begins when OPENXCOM3D_ANIMATION_CHECK names a file: every animated
 * record (a tile part whose eight frames aren't one picture) of every tile set the mod knows, its terrains', its
 * UFOs' and its crafts', and whether its animation shows in the view, as the view would build it with the settings
 * that are on. Written to the file, a line a record, tab between the fields:
 *   set, record, part (floor, west, north, object), pictures, the share of its drawn pixels that changes a step
 *   (%), its voxels, the steps of its art (frames that differ from the one before) and how many of them show (the
 *   two frames' models differ), the share of its voxels that changes a step (%), the kind of ghost it is (0: it has
 *   voxels), "light" or "motion" (see pulsingFrames), and the verdict:
 *   animates  every step of its art shows;
 *   partly    some steps show, some are the same model twice;
 *   frozen    no step shows: the art changes where the model has nothing (above or beside the game's shape);
 *   unseen    it is not drawn at all (no voxels and no ghost of it);
 *   sprite    a flame, drawn as its sprite and animated so (see partKind);
 *   door      a sliding door: its frames are its opening (see snapshotTile);
 *   animates+sprite  ... and the art outside its shape that animates is drawn as a sprite (see artOverlay);
 *   lights    a UFO's part with the setting "UFO lights": a steady model whose lights pulse (so many voxels);
 *   steady+sprite  ... without lights, its models all alike: the art outside its shape that animates is a sprite;
 *   steady    ... and nothing of it moves.
 * @param path The file to write.
 */
void FirstPersonView::checkAnimatedParts(const char *path)
{
	Mod *mod = _game->getMod();
	std::vector<MapDataSet*> sets = everyTileSet(mod);

	FILE *out = fopen(path, "w");
	if (!out)
	{
		return;
	}
	std::map<std::string, int> verdicts;
	const std::vector<Uint16> *loftData = mod->getVoxelData();
	static const char *const partNames[] = { "floor", "west", "north", "object" };
	for (MapDataSet *set : sets)
	{
		try
		{
			set->loadData(mod->getMCDPatch(set->getName()));
		}
		catch (const std::exception &)
		{
			fprintf(out, "%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\tnot loaded\n", set->getName().c_str());
			continue;
		}
		SurfaceSet *art = set->getSurfaceset();
		for (size_t record = 0; art && record < set->getSize(); ++record)
		{
			const MapData *mapData = set->getObject(record);
			const Frames &frames = getFrames(mapData);
			if (!frames.animated)
			{
				continue;
			}
			int pictures[8], pictureCount = 0;
			for (int frame = 0; frame < 8; ++frame)
			{
				if (std::find(pictures, pictures + pictureCount, frames.sprite[frame]) == pictures + pictureCount) pictures[pictureCount++] = frames.sprite[frame];
			}
			// The art's steps, and how much of the picture each changes.
			int artSteps = 0, shownSteps = 0, voxels = 0;
			double artShare = 0.0, voxelShare = 0.0;
			const bool door = mapData->isUFODoor();
			bool ufoPart = _ufoDataSets.count(set->getName()) && Options::fpsUfoLights;
			int lit = 0;
			if (ufoPart && !door)
			{
				UfoModel steady;
				const float nowhere[2] = { 0.0f, 0.0f };
				buildUfoModel(mapData, nowhere, steady);
				for (int z = 0; z < Position::TileZ; ++z)
					for (int y = 0; y < Position::TileXY; ++y)
						for (int x = 0; x < Position::TileXY; ++x) lit += (steady.lit[z][y] >> x) & 1;
				const VoxelModel *shape = getModel(mapData, frames.sprite[0], false);
				ufoPart = !shape->empty && (lit > 0 || !modelsAnimate(mapData)) && turnedFrom(mapData) < 0; // (as findUfos gives out the steady models)
			}
			bool anyDrawn = false;
			int ghostKind = 0;
			for (int frame = 0; frame < 8; ++frame)
			{
				const int next = (frame + 1) % 8;
				const Surface *from = art->getFrame(frames.sprite[frame]), *to = art->getFrame(frames.sprite[next]);
				if (frames.sprite[frame] == frames.sprite[next] || !from || !to || from->getWidth() != to->getWidth() || from->getHeight() != to->getHeight())
				{
					continue;
				}
				int drawn = 0, changed = 0;
				for (int y = 0; y < from->getHeight(); ++y)
				{
					for (int x = 0; x < from->getWidth(); ++x)
					{
						const Uint8 before = from->getPixel(x, y), after = to->getPixel(x, y);
						drawn += before || after;
						changed += before != after;
					}
				}
				if (!changed)
				{
					continue; // (two numbers for one picture)
				}
				++artSteps;
				artShare += drawn ? changed / (double)drawn : 0.0;
				if (door || ufoPart)
				{
					continue;
				}
				// The two frames as the view builds them: their voxels, and those of their own ghosts.
				const VoxelModel *models[2] = { getModel(mapData, frames.sprite[frame], false), getModel(mapData, frames.sprite[next], false) };
				int solid = 0, differing = 0;
				for (int layer = 0; layer < 2; ++layer)
				{
					const VoxelModel *now = layer ? models[0]->ghost : models[0], *then = layer ? models[1]->ghost : models[1];
					if (!now && !then) continue;
					for (int z = 0; z < Position::TileZ; ++z)
						for (int y = 0; y < Position::TileXY; ++y)
							for (int x = 0; x < Position::TileXY; ++x)
							{
								const Uint8 a = now && !now->empty ? now->color[z][y][x] : 0, b = then && !then->empty ? then->color[z][y][x] : 0;
								solid += a || b;
								differing += a != b;
							}
				}
				anyDrawn = anyDrawn || solid;
				ghostKind = std::max<int>(ghostKind, models[0]->ghostKind);
				voxels = std::max(voxels, solid);
				shownSteps += differing > 0;
				voxelShare += solid ? differing / (double)solid : 0.0;
			}
			if (!artSteps)
			{
				continue;
			}
			std::string verdict;
			const bool overlay = !door && Options::fpsGhostParts != 0 && artOverlay(mapData) != nullptr;
			if (door)
			{
				verdict = "door";
			}
			else if (ufoPart)
			{
				voxels = lit;
				verdict = lit ? "lights" : overlay ? "steady+sprite" : "steady";
			}
			else if (!anyDrawn)
			{
				const VoxelModel *kind = partKind(mapData, false);
				verdict = (kind && kind->ghostKind == 5) || overlay ? "sprite" : "unseen";
			}
			else if (overlay)
			{
				verdict = "animates+sprite"; // (its art outside its shape is drawn as a sprite: see artOverlay)
			}
			else
			{
				verdict = shownSteps == artSteps ? "animates" : shownSteps ? "partly" : "frozen";
			}
			++verdicts[verdict];
			fprintf(out, "%s\t%d\t%s\t%d\t%.1f\t%d\t%d\t%d\t%.1f\t%d\t%s\t%s\n", set->getName().c_str(), (int)record,
				mapData->getObjectType() >= 0 && mapData->getObjectType() < 4 ? partNames[mapData->getObjectType()] : "?", pictureCount,
				100.0 * artShare / artSteps, voxels, artSteps, shownSteps, 100.0 * voxelShare / artSteps, ghostKind,
				hasNoVoxels(mapData, *loftData) ? "ghost" : pulsingFrames(mapData) ? "light" : "motion", verdict.c_str());
		}
	}
	fclose(out);
	std::string summary;
	for (const auto &verdict : verdicts) summary += " " + verdict.first + " " + std::to_string(verdict.second);
	Log(LOG_INFO) << "OpenXcom3D: animated parts in " << sets.size() << " tile sets:" << summary << " (written to " << path << ")";
}


/// A developer's arena: the spot of its fifteen left empty (the middle one).
static const int ArenaEmptySpot = 7;

/**
 * A developer's arena, run while OPENXCOM3D_ANIMATION_ARENA names a path: what checkAnimatedParts tells of the models,
 * seen on the screen. Every animated record of every tile set the mod knows (but the sliding doors, whose frames are
 * their opening) stands, twelve at a time, on a patch of the battle's map cleared from the ground up, floored and in
 * daylight, before a camera fixed above it: rows of five, five and three spots, one of them left empty to tell what
 * else changes on the screen. A script outside asks for the next batch by making the file <path>.go, takes pictures
 * of the screen over a whole cycle of the animation and measures what changes where each part stands; where that is
 * on the screen is written to <path>.tsv, a line a spot (batch, spot, set, record, part, left, top, right, bottom; the
 * empty spot's set is "-", a spot not used "x"), and "done" after the last batch. No units are drawn meanwhile, and
 * the battle is no good for playing after.
 * Two files change what is shown: <path>.only lists the records to show instead, animated or not, a line each (set,
 * record); with <path>.sparse there, a batch is three parts far apart, out of the reach of each other's light.
 * (A part of a set UFOs are built from stands here alone, which is no UFO: it plays its frames, as with the setting
 * "UFO lights" off; its steady model with pulsing lights shows inside a UFO only.)
 * And with a file <path>.save there, holding a page's number, nothing of the above: the parts are laid all over the
 * map for a player to walk among, and written as a save (see arenaSave).
 * Called as a frame begins, before the camera's axes are made.
 */
void FirstPersonView::arenaStep()
{
	static const char *const path = getenv("OPENXCOM3D_ANIMATION_ARENA");
	if (!path || !_save || _sharedModelsOwner || getWidth() < 320 || _arena.failed)
	{
		return;
	}
	Arena &arena = _arena;
	// (Spots two tiles apart in a row, the rows four and three tiles apart, the camera four tiles before the nearest:
	// seen from that high, the box round a tile reaches over the ground behind it to a third again of its distance,
	// and no part's place on the screen holds another's. The nearest row's outer spots would be off the screen.)
	static const int Columns = 5, Rows = 3, ColumnStep = 2, Back = 4, Margin = 2, FarMargin = 5;
	static const int RowAt[Rows] = { 0, 4, 7 };
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY(), sizeZ = _save->getMapSizeZ();
	if (!arena.ready)
	{
		Mod *mod = _game->getMod();
		std::set<std::pair<std::string, int>> only;
		bool listed = false;
		if (FILE *list = fopen((std::string(path) + ".only").c_str(), "r"))
		{
			listed = true;
			char name[128];
			int record = 0;
			while (fscanf(list, "%127s %d", name, &record) == 2) only.emplace(name, record);
			fclose(list);
		}
		if (FILE *sparse = fopen((std::string(path) + ".sparse").c_str(), "r"))
		{
			arena.sparse = true;
			fclose(sparse);
		}
		for (MapDataSet *set : everyTileSet(mod))
		{
			try
			{
				set->loadData(mod->getMCDPatch(set->getName()));
			}
			catch (const std::exception &)
			{
				continue;
			}
			for (size_t record = 0; set->getSurfaceset() && record < set->getSize(); ++record)
			{
				MapData *mapData = set->getObject(record);
				const bool shown = listed ? only.count(std::make_pair(set->getName(), (int)record)) > 0 : getFrames(mapData).animated && !mapData->isUFODoor();
				if (shown) arena.parts.emplace_back(mapData, (int)record);
			}
		}
		for (int index = 0; index < _save->getMapSizeXYZ() && !arena.floor; ++index)
		{
			MapData *floor = _save->getTile(index)->getMapData(O_FLOOR);
			if (floor && !floor->isGravLift() && !getFrames(floor).animated && !getModel(floor, getFrames(floor).sprite[0], false)->empty)
			{
				arena.floor = floor;
				_save->getTile(index)->getMapData(&arena.floorRecord, &arena.floorSet, O_FLOOR);
			}
		}
		if (FILE *asked = fopen((std::string(path) + ".save").c_str(), "r"))
		{
			int page = 1;
			if (fscanf(asked, "%d", &page) != 1) page = 1;
			fclose(asked);
			arena.failed = true; // (nothing more of the arena after it)
			if (arena.floor) arenaSave(path, std::max(1, page));
			return;
		}
		const int wide = (Columns - 1) * ColumnStep + 1 + 2 * Margin, deep = FarMargin + RowAt[Rows - 1] + 1 + Back + 1 + Margin;
		if (sizeX < wide || sizeY < deep || sizeZ < 3 || !arena.floor)
		{
			Log(LOG_ERROR) << "OpenXcom3D: no room for the animation arena on this map";
			arena.failed = true;
			return;
		}
		arena.origin = Position((sizeX - wide) / 2 + Margin, (sizeY - deep) / 2 + FarMargin, 0);
		// The patch: all of the map seen, nothing on it from the ground up but the floor, in daylight.
		_save->setDebugMode();
		for (int z = 0; z < sizeZ; ++z)
			for (int y = arena.origin.y - FarMargin; y < arena.origin.y - FarMargin + deep; ++y)
				for (int x = arena.origin.x - Margin; x < arena.origin.x - Margin + wide; ++x)
				{
					Tile *tile = _save->getTile(Position(x, y, z));
					for (int part = 0; part < 4; ++part) tile->setMapData(z == 0 && part == O_FLOOR ? arena.floor : nullptr, -1, -1, (TilePart)part);
					tile->setSmoke(0);
					tile->setFire(0);
				}
		_save->setGlobalShade(0);
		_save->getTileEngine()->calculateLighting(LL_AMBIENT, TileEngine::invalid, 0, true);
		if (FILE *out = fopen((std::string(path) + ".tsv").c_str(), "w")) fclose(out);
		Log(LOG_INFO) << "OpenXcom3D: the animation arena is ready, " << arena.parts.size() << " parts to show";
		arena.ready = true;
	}
	// The camera: above the middle of the rows' width, before the nearest, looking north and down, half way between
	// the nearest row's foot and the farthest's top.
	const double eyeHeight = std::min(sizeZ * (double)Position::TileZ - 2.0, 90.0);
	setEye((arena.origin.x + (Columns - 1) * ColumnStep * 0.5 + 0.5) * Position::TileXY, (arena.origin.y + RowAt[Rows - 1] + 0.5 + Back) * Position::TileXY, eyeHeight);
	_yaw = -Pi / 2.0;
	_pitch = -0.5 * (std::atan2(eyeHeight, (Back - 0.5) * Position::TileXY) + std::atan2(eyeHeight - Position::TileZ, (Back + RowAt[Rows - 1] + 0.5) * Position::TileXY));
	// The next batch, when the script asks.
	const std::string go = std::string(path) + ".go";
	FILE *asked = fopen(go.c_str(), "r");
	if (!asked)
	{
		return;
	}
	fclose(asked);
	remove(go.c_str());
	++arena.batch;
	arena.spots.clear();
	for (int spot = 0; spot < Columns * Rows; ++spot)
	{
		const Position at(arena.origin.x + (spot % Columns) * ColumnStep, arena.origin.y + RowAt[spot / Columns], 0);
		Tile *tile = _save->getTile(at);
		for (int part = 0; part < 4; ++part) tile->setMapData(part == O_FLOOR ? arena.floor : nullptr, -1, -1, (TilePart)part);
		const bool offScreen = (spot / Columns == Rows - 1 && (spot % Columns == 0 || spot % Columns == Columns - 1))
			|| (arena.sparse && spot != 0 && spot != Columns - 1 && spot != (Rows - 1) * Columns + Columns / 2);
		Arena::Spot laid = { at, nullptr, -1 };
		if (spot != ArenaEmptySpot && !offScreen && arena.next < arena.parts.size())
		{
			const std::pair<MapData*, int> &entry = arena.parts[arena.next++];
			tile->setMapData(entry.first, entry.second, 0, entry.first->getObjectType());
			artOverlay(entry.first); // (a set this battle hasn't got was not looked at for its sprites of art where the battle began)
			laid.part = entry.first;
			laid.record = entry.second;
		}
		arena.spots.push_back(laid);
	}
	// The view starts on the map afresh (no rubble flies for the parts gone), finds its UFOs' parts and its lifts
	// again, and the parts with a sprite of art.
	_wallsLastFrame[0].clear();
	findUfos();
	gpuForgetModels(); // (findUfos makes the UFO parts' steady models anew: the card knows a model by where it is kept)
	findLifts();
	_overlaySpotsAge = 0;
	arena.toLog = true;
}

/**
 * A developer's arena (see arenaStep): once a batch is laid and the camera's axes are made, writes where each of its
 * spots is on the screen, the box round its tile.
 */
void FirstPersonView::arenaLog()
{
	if (!_arena.toLog)
	{
		return;
	}
	_arena.toLog = false;
	FILE *out = fopen((std::string(getenv("OPENXCOM3D_ANIMATION_ARENA")) + ".tsv").c_str(), "a");
	if (!out)
	{
		return;
	}
	static const char *const partNames[] = { "floor", "west", "north", "object" };
	bool any = false;
	for (size_t spot = 0; spot < _arena.spots.size(); ++spot)
	{
		const Arena::Spot &laid = _arena.spots[spot];
		double low[2] = { 1e9, 1e9 }, high[2] = { -1e9, -1e9 };
		for (int corner = 0; corner < 8; ++corner)
		{
			const double world[3] = { (laid.at.x + (corner & 1)) * (double)Position::TileXY, (laid.at.y + ((corner >> 1) & 1)) * (double)Position::TileXY,
				(laid.at.z + ((corner >> 2) & 1)) * (double)Position::TileZ };
			double screen[2], depth;
			if (!projectToScreen(world, screen[0], screen[1], depth)) continue;
			for (int axis = 0; axis < 2; ++axis)
			{
				low[axis] = std::min(low[axis], screen[axis]);
				high[axis] = std::max(high[axis], screen[axis]);
			}
		}
		any = any || laid.part;
		fprintf(out, "%d\t%d\t%s\t%d\t%s\t%d\t%d\t%d\t%d\n", _arena.batch, (int)spot, laid.part ? laid.part->getDataset()->getName().c_str() : (int)spot == ArenaEmptySpot ? "-" : "x", laid.record,
			laid.part ? partNames[laid.part->getObjectType()] : "-", (int)std::floor(low[0]), (int)std::floor(low[1]), (int)std::ceil(high[0]), (int)std::ceil(high[1]));
	}
	if (!any)
	{
		fprintf(out, "done\n");
	}
	fclose(out);
}

/**
 * A developer's arena as a save to walk through (see arenaStep; the owner: "Make me also an arena save i can revisit
 * with my own eyes"): the whole map cleared down to one floor on the ground, in daylight and all seen; the battle's
 * units stood on the ground, the enemy's without time units (no shots at who walks by); and a page of the animated
 * parts laid on every other tile each way, in the order of the arena's list, row by row from the rows nearest the
 * player's soldier, a page holding as many as the map has such tiles free. The parts' sets join the battle's, so the save holds them. Written as arena_<page>.sav in the
 * user's folder, with <path>.list.tsv beside the arena's files: tile x, tile y, set, record, part, a line a part.
 * @param path The arena's path.
 * @param page Which page of the parts, from 1.
 */
void FirstPersonView::arenaSave(const char *path, int page)
{
	Arena &arena = _arena;
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	for (int index = 0; index < _save->getMapSizeXYZ(); ++index)
	{
		Tile *tile = _save->getTile(index);
		const bool ground = tile->getPosition().z == 0;
		for (int part = 0; part < 4; ++part)
		{
			if (ground && part == O_FLOOR) tile->setMapData(arena.floor, arena.floorRecord, arena.floorSet, O_FLOOR);
			else tile->setMapData(nullptr, -1, -1, (TilePart)part);
		}
		tile->setSmoke(0);
		tile->setFire(0);
	}
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->isOut()) continue;
		if (unit->getFaction() != FACTION_PLAYER) unit->clearTimeUnits();
		const Position at = unit->getPosition();
		bool placed = at.z == 0;
		for (int reach = 0; reach < 16 && !placed; ++reach)
			for (int dy = -reach; dy <= reach && !placed; ++dy)
				for (int dx = -reach; dx <= reach && !placed; ++dx) placed = _save->setUnitPosition(unit, Position(at.x + dx, at.y + dy, 0));
	}
	std::vector<Position> spots;
	for (int y = 2; y < sizeY - 2; y += 2)
		for (int x = 2; x < sizeX - 2; x += 2)
			if (!_save->getTile(Position(x, y, 0))->getUnit()) spots.emplace_back(x, y, 0);
	// (The rows nearest the player's soldier first, each from the west: a page that doesn't fill the map stands round them.)
	const BattleUnit *soldier = _save->getSelectedUnit();
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (!soldier && !unit->isOut() && unit->getFaction() == FACTION_PLAYER) soldier = unit;
	}
	const int soldierRow = soldier ? soldier->getPosition().y : sizeY / 2;
	std::stable_sort(spots.begin(), spots.end(), [&](const Position &a, const Position &b) { return std::abs(a.y - soldierRow) < std::abs(b.y - soldierRow); });
	const size_t first = (size_t)(page - 1) * spots.size();
	std::vector<MapDataSet*> *sets = _save->getMapDataSets();
	static const char *const partNames[] = { "floor", "west", "north", "object" };
	FILE *list = fopen((std::string(path) + ".list.tsv").c_str(), "w");
	size_t laid = 0;
	for (; laid < spots.size() && first + laid < arena.parts.size(); ++laid)
	{
		const std::pair<MapData*, int> &entry = arena.parts[first + laid];
		MapDataSet *set = entry.first->getDataset();
		const size_t setIndex = std::find(sets->begin(), sets->end(), set) - sets->begin();
		if (setIndex == sets->size()) sets->push_back(set);
		_save->getTile(spots[laid])->setMapData(entry.first, entry.second, (int)setIndex, entry.first->getObjectType());
		artOverlay(entry.first);
		if (list) fprintf(list, "%d\t%d\t%s\t%d\t%s\n", spots[laid].x, spots[laid].y, set->getName().c_str(), entry.second, partNames[entry.first->getObjectType()]);
	}
	if (list) fclose(list);
	_save->revealMap();
	_save->setGlobalShade(0);
	_save->getTileEngine()->calculateLighting(LL_AMBIENT, TileEngine::invalid, 0, true);
	_wallsLastFrame[0].clear();
	findUfos();
	gpuForgetModels();
	findLifts();
	_overlaySpotsAge = 0;
	const std::string name = "arena_" + std::to_string(page) + ".sav";
	SavedGame *game = _game->getSavedGame();
	const std::string was = game->getName();
	game->setName("Animated parts, page " + std::to_string(page));
	game->save(name, _game->getMod());
	game->setName(was);
	Log(LOG_INFO) << "OpenXcom3D: the arena's save " << name << " is written: parts " << first + 1 << " to " << first + laid << " of " << arena.parts.size()
		<< ", " << spots.size() << " a page";
}
}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
