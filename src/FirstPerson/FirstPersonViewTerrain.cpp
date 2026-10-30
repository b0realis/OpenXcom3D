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
#include <algorithm>
#include <array>
#include <bitset>
#include <climits>
#include <cmath>
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
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleTerrain.h"
#include "../Mod/RuleUfo.h"
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
	std::fill(&model.clear[0][0], &model.clear[0][0] + 24 * 16, (Uint16)0);

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
	model.water = false;
	model.liquidHeight = 0;
	model.artFill = false;
	model.cropBlock = 0;
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

	// The voxels: each where its pixel is drawn, with that pixel's colour.
	auto place = [&](int x, int y, int z)
	{
		const Uint8 color = pixelOf(x, y, z);
		if (!color) return;
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
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x) place(x, y, 0);
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
	const Uint32 startTick = SDL_GetTicks();

	// The names of the map data sets that crafts and UFOs are built from, minus any that an
	// ordinary terrain uses too.
	std::set<std::string> craftSets, terrainSets;
	auto collect = [](RuleTerrain *terrain, std::set<std::string> &into)
	{
		if (!terrain) return;
		for (const MapDataSet *dataSet : *terrain->getMapDataSets()) into.insert(dataSet->getName());
	};
	for (const auto &type : mod->getCraftsList()) collect(mod->getCraft(type)->getBattlescapeTerrainData(), craftSets);
	for (const auto &type : mod->getUfosList()) collect(mod->getUfo(type)->getBattlescapeTerrainData(), craftSets);
	for (const auto &type : mod->getTerrainList()) collect(mod->getTerrain(type), terrainSets);
	for (const auto &setName : terrainSets) craftSets.erase(setName); // (BLANKS and the like are everywhere)
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
 * @return The palette colour, 0 if nothing is there or the voxel is outside the map.
 */
Uint8 FirstPersonView::voxelColorAt(int worldX, int worldY, int worldZ, bool nextFrame, int onlyPart) const
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
		const Uint8 color = voxelColorAt(sourceVoxel[0], sourceVoxel[1], sourceVoxel[2], nextFrame, onSkin ? -1 : hit.part); // (0 if it has been blown away since)
		if (color && !isBlack(color)) return color;
	}
	return 0;
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
		// Not built yet: build it in place (the map keeps it at the same address from then on).
		found = _models.emplace(key, VoxelModel()).first;
		buildModel(mapData, spriteIndex, cutout, found->second);
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
void FirstPersonView::buildModel(const MapData *mapData, int spriteIndex, bool cutout, VoxelModel &model) const
{
	clearModel(model);

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
	const bool waterFloor = mapData->getObjectType() == O_FLOOR && firstPerson.isWater(dataSet->getName());
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
	const bool hasArtHoles = (mapData->getObjectType() == O_WESTWALL || mapData->getObjectType() == O_NORTHWALL) && !hasHollows
		&& !_ufoDataSets.count(dataSet->getName()) && findArtHoles(sprite, artHoles);

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
					model.glass[z][y] |= (Uint16)(1 << x);
				}
				if (hasArtHoles && spriteY >= 0 && spriteY < sprite->getHeight() && artHoles[spriteY * sprite->getWidth() + spriteX])
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
		const bool low = mapData->getObjectType() == O_OBJECT && lowArt(sprite, yOffset, rises);
		const int block = cutout ? 0 : cropBlockOf(nullptr, mapData, sprite, yOffset);
		const bool ghostWanted = Options::fpsGhostParts == 2
			|| (Options::fpsGhostParts == 1 && (ghostShape || mapData->isGravLift() || cageArt(sprite, mapData->getObjectType(), yOffset) || low || block));
		if (ghostWanted && !cutout)
		{
			if (ghostShape)
			{
				buildShape(model, sprite, *shape, yOffset);
			}
			else if (block)
			{
				buildCropBlock(model, sprite, block, yOffset);
			}
			else if (low)
			{
				buildRelief(model, sprite, yOffset, rises);
			}
			else
			{
				buildGhost(model, sprite, mapData->getObjectType(), yOffset);
			}
			if (!model.empty) colorHiddenSides(model, sprite);
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

	// A thin plate (all its voxels in the bottom 4 layers) may be drawn as a raised slab: fit its top.
	if (!model.empty && model.max[2] <= 3 && !cutout)
	{
		fitPlateTopToSprite(model, sprite, yOffset, nudgeCount);
	}

	// A floor drawn as a box of liquid that slows walking (XPiratez's sewer sludge, see
	// liquidHeightOf): the game's thin slab stays the floor, its top (the bottom under the liquid)
	// a few shades darker; the liquid's surface is a ghost model of its own (see getModel).
	const int walkCost = mapData->getTUCost(MT_WALK);
	if (mapData->getObjectType() == O_FLOOR && !model.empty && model.max[2] <= 3 && !cutout
		&& walkCost > PlainFloorWalkCost && walkCost < Pathfinding::INVALID_MOVE_COST)
	{
		model.liquidHeight = (Uint8)liquidHeightOf(sprite, yOffset);
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
		model.artFill = sparseArt(model, sprite, mapData->getObjectType(), yOffset);
	}
	// An object the art draws as a block of crops though the game gives it only a few voxels
	// (X-Com Files' wheat: see cropBlockOf): the block is a ghost model of its own (see getModel).
	if (!model.empty && !cutout)
	{
		model.cropBlock = (Uint8)cropBlockOf(&model, mapData, sprite, yOffset);
	}
	// A mass of foliage: drawn leafy on a tile without a floor (see foliageArt, leafyModel).
	model.foliage = mapData->getObjectType() == O_OBJECT && !cutout && foliageArt(model, sprite, getPalette());
	colorHiddenSides(model, sprite);
	findGlow(model);
	model.water = waterFloor && !model.empty;
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
 *   and goes on underground, the surface's level. Beyond the border, below the surface, is rock;
 * - whether the map is underground all the way up (every level of the ring is rock: a base, an
 *   alien colony's inside, Cydonia's complex), or its mission is listed as underground in the
 *   mod (RuleFirstPerson::isUnderground): then rock lies beyond the border at every height and
 *   over the map too, where it would otherwise show the sky (or, under the sea, the water);
 * - the ground that runs on past the border at the surface's level: the most common floor of the
 *   ring there (grass on a farm, sand in the desert, ...). Only a floor whose model reaches down
 *   to the bottom voxel layer counts;
 * - the water: where a floor of the mod's water (RuleFirstPerson::isWater: TFTD's sea) reaches
 *   the border, the sea goes on beyond that stretch of it, the most common water floor of the
 *   ring (an island's shore runs on out of the map, the sea round a ship); the ground runs on
 *   past the rest. A ring of water alone (round a ship) is the ground too. The sea's colours
 *   for its waves (see waterColor) come from that floor's top, or with no water at the edge
 *   from the commonest water floor on the map.
 * Sets _surfaceLevel, _enclosed, _groundModel (left as it was if no floor qualifies),
 * _waterModel, _waterAtEdge, _waterColors and _groundChosen.
 */
void FirstPersonView::chooseGround()
{
	_groundChosen = true;
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
	// The rock's colour: the commonest colour group among the walls and objects round the map's
	// underground levels (the rock it's cut into, in whatever palette the battle has: a fixed
	// brown came out olive or watery blue under the sea), at a middle shade, with room to shade
	// it either way. Every third voxel of their models is enough to tell.
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
	int rockCount = 0;
	for (const auto &group : rockGroups)
	{
		if (group.second > rockCount)
		{
			rockCount = group.second;
			_rockBase = (Uint8)(group.first | RockBaseShade);
		}
	}
	Log(LOG_VERBOSE) << "OpenXcom3D: " << _save->getMissionType() << ": " << (_enclosed ? "underground all the way up (rock round the map and over it)"
		: "the surface at the map's edges is level " + std::to_string(_surfaceLevel) + " (rock beyond the edges below it)");

	// Count the floors of the ring at the surface's level (the bottom one on an enclosed map,
	// which shows no ground beyond the border anyway), the water's apart, and note where the
	// water is (by the tile's x and y).
	const RuleFirstPerson &rules = _save->getMod()->getFirstPerson();
	std::map<const MapData*, int> floorCounts, waterCounts;
	_waterAtEdge.assign(mapSizeX * mapSizeY, 0);
	forEachRingTile(std::min(_surfaceLevel, mapSizeZ - 1), [&](const Tile *tile)
	{
		if (const MapData *floor = tile->getMapData(O_FLOOR))
		{
			const bool water = rules.isWater(floor->getDataset()->getName());
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
	_waterModel = commonest(waterCounts);
	if (const VoxelModel *ground = commonest(floorCounts))
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
				if (floor && rules.isWater(floor->getDataset()->getName())) mapWaterCounts[floor]++;
			}
		seaModel = commonest(mapWaterCounts);
	}
	chooseWaterColors(seaModel);
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
	const Uint8 plain = seaModel->ceiling;
	// The deep colour: count the top's colours of other groups, darker than the plain one.
	int counts[256] = {};
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
		{
			const Uint8 color = seaModel->plan[y][x];
			if (color && (color & 0xF0) != (plain & 0xF0) && luminance(color) < luminance(plain) && luminance(color) >= DarkGlowLuma) counts[color]++;
		}
	const int deepest = (int)(std::max_element(counts, counts + 256) - counts);
	const Uint8 deep = counts[deepest] ? (Uint8)deepest : (Uint8)((plain & 0xF0) | std::min(13, (plain & 15) + 3));
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
	_nearbyLights.clear();
	_nearbyDevices.clear();
	_tileFlags.resize(tileCount);
	_tileLights.resize(tileCount);
	for (int index = 0; index < tileCount; ++index)
	{
		Tile *tile = _save->getTile(index);
		if (trackWalls)
		{
			noticeTileChanges(index, tile);
		}
		else
		{
			startWatchingTile(index, tile);
		}
		snapshotTile(index, tile, trackWalls);
		const TileView &tileView = _tileViews[index];
		noteLightsAndDevices(tile, tileView);
		const int top = tileView.max[2] >= tileView.min[2] ? tileView.max[2] : 0;
		_tileFlags[index] = (Uint8)((tileView.empty ? 0 : TileTraced) | (tileView.smoke ? TileSmoky : 0) | (top << TileTopShift));
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
	updateDoorSwings(tileCount);
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
	_solidMasks.clear();
	_paintedFloors.clear();
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
 */
void FirstPersonView::noticeTileChanges(int index, Tile *tile)
{
	trackDoors(index, tile);

	// A fire burnt out here: the ground is left blackened (a scorch mark of radius 10 voxels on
	// the floor's surface in the middle of the tile; at most 200 of them).
	const bool burning = tile->getFire() > 0;
	if (_burningLastFrame[index] && !burning && _scorches.size() < 200)
	{
		const Position tilePos = tile->getPosition();
		_scorches.push_back({ { tilePos.x * 16.0f + 8.0f, tilePos.y * 16.0f + 8.0f, tilePos.z * 24.0f + GroundTop - tile->getTerrainLevel() },
			10.0f });
	}
	_burningLastFrame[index] = burning;

	// Something destroyed here since last frame (not a door opening): debris. A part counts as
	// destroyed when it vanished or turned into its destroyed version (the record's "die MCD").
	for (int part = 0; part < 4; ++part)
	{
		const MapData *partBefore = _partsLastFrame[part][index], *partNow = tile->getMapData((TilePart)part);
		_partsLastFrame[part][index] = partNow;
		if (!partBefore || partBefore == partNow || partBefore->isDoor() || partBefore->isUFODoor() || !partBefore->getDataset()) continue;
		const MapData *destroyedVersion = partBefore->getDieMCD() ? partBefore->getDataset()->getObject(partBefore->getDieMCD()) : nullptr;
		if (partNow && partNow != destroyedVersion) continue;
		// dx, dy: from the camera to the middle of the tile, in voxels.
		const Position tilePos = tile->getPosition();
		const double dx = tilePos.x * 16.0 + 8.0 - _eye[0], dy = tilePos.y * 16.0 + 8.0 - _eye[1];
		if (dx * dx + dy * dy < 480.0 * 480.0) spawnDebris(tilePos, part, partBefore);
	}
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
	for (int part = 0; part < 4; ++part) _partsLastFrame[part][index] = tile->getMapData((TilePart)part);
	_burningLastFrame[index] = tile->getFire() > 0;
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
	const bool debug = _save->getDebugMode();
	TileView &tileView = _tileViews[index];
	tileView.empty = true;
	tileView.swing[0] = tileView.swing[1] = -1;
	tileView.emissive = 0;
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
		// part the eye is inside (Doom IDCLIP).
		const MapData *mapData = tile->getMapData((TilePart)part);
		if (!mapData || (part == O_FLOOR && (int)index == _hiddenFloor)
			|| (!_leftOutParts.empty() && std::find(_leftOutParts.begin(), _leftOutParts.end(), std::make_pair(index, part)) != _leftOutParts.end()))
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
			frame = age < UfoDoorTime ? std::clamp(1 + (int)(age * 6 / UfoDoorTime), 1, 7) : 7;
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
		const VoxelModel *model = memo.model;
		if (model->empty)
		{
			continue;
		}
		tileView.part[part] = model;
		tileView.next[part] = memo.next;
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

	// The game's shade for the tile (with night vision no darker than 4), and the fog of
	// war: the floor and the object are seen with the floor, each wall on its own (a wall is
	// seen from either side, as on the isometric map).
	tileView.light = (Uint8)std::clamp(tile->getShade(), 0, _nightVision ? 4 : 15);
	// A UFO's light (the UFO lights option), once that UFO has been seen (see findUfos).
	tileView.ufoGlow = _ufoLightsOn && _ufoOfTile[index] && _ufos[_ufoOfTile[index] - 1].seen ? _ufoGlowOfTile[index] : 0;
	tileView.discovered = debug || tile->isDiscovered(O_FLOOR);
	tileView.seen = (Uint8)((tileView.discovered ? (1 << O_FLOOR) | (1 << O_OBJECT) : 0)
		| (debug || tile->isDiscovered(O_WESTWALL) ? 1 << O_WESTWALL : 0)
		| (debug || tile->isDiscovered(O_NORTHWALL) ? 1 << O_NORTHWALL : 0));
	// Its smoke, once the tile has been seen (as the map shows smoke only there), and whether
	// it burns.
	tileView.smoke = tileView.discovered ? (Uint8)std::clamp(tile->getSmoke(), 0, 255) : 0;
	tileView.burning = tile->getFire() > 0;
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
		tileView.part[door->hiddenPart] = nullptr;
		tileView.next[door->hiddenPart] = nullptr;
		addSwingTo(door->tile, (Sint16)_doorSwings.size());
		_doorSwings.push_back(swing);
		++door;
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
