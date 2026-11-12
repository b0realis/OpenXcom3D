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
 * FirstPersonDamageNumbers.cpp: setting "Damage numbers" (the owner's): when a unit is hit, a
 * number rises over it and fades away in about a second and a half, in the 3D view (in the world,
 * over the unit's head: walls and units in front hide it, as anything there; a fixed size on the
 * screen, so it reads far off too) and on the isometric map (over the unit's sprite). The numbers
 * are the game's own, read where it deals the damage (TileEngine::hitUnit, the one place every
 * shot, blow and blast hurts a unit): the health the unit lost, after its armour, in red; the armour
 * points the hit took off the side it struck, in blue; the stun it took, in purple; side by side in
 * that order, each only when there is some; a grey 0 for a hit that did none of these (its armour
 * stopped it completely). Every hit gets one, whoever struck (reaction fire too), each unit a blast
 * hurts its own, but only on units the squad sees (see FirstPersonMode::unitDamaged). Numbers that would overlap step up over the newer ones
 * (a unit's hits close together stack upwards, the newest at the bottom; a blast's over units side
 * by side), so each stays readable. The game's small font, in the palette's nearest colours to a
 * clear red, blue, purple and grey, outlined in black.
 */
#include "FirstPersonDamageNumbers.h"
#include <algorithm>
#include <climits>
#include <string>
#include "../Engine/Font.h"
#include "../Engine/Surface.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// How far a number rises over its time (screen pixels, scale 1).
const int DamageNumberRise = 12;
/// The share of its time a number shows whole; over the rest it dissolves away.
const double DamageNumberSolidShare = 0.6;
/// Pixels between one hit's numbers (scale 1).
const int DamageNumberGap = 2;
/// How much nearer than the number something in the 3D view must be to hide it (voxels): the
/// unit's own picture just under it doesn't.
const double DamageNumberDepthMargin = 6.0;
/// How far under a box kept clear a number's way up must stay to rise there (scale 1), else it starts over it.
const int KeepClearMargin = 16;
/// The order the pixels dissolve in as a number fades (a 4x4 ordered dither: the highest go first).
const int DissolveOrder[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/// What a cell of a number's picture is.
enum NumberCell { CELL_NONE, CELL_OUTLINE, CELL_HEALTH, CELL_ARMOR, CELL_STUN, CELL_SHIELD, CELL_NO_HARM };

/// A hit's numbers laid out in the small font, one cell a pixel (scale 1).
struct NumberPicture
{
	int width = 0, height = 0;
	std::vector<Uint8> cells; // NumberCell, rows of `width`
};

/**
 * Lays a hit's numbers out in the small font, as the game's Text lays text out (each glyph its own
 * width, the font's spacing between them): the health lost, the armour taken off and the stun
 * taken side by side (only those there are), or a 0. The small font's glyphs come outlined: their strokes are its colour's
 * first two shades (1, 2), the border round them darker ones; a stroke wins where a glyph's border
 * overlaps the next one's.
 * @param font The small font.
 * @param number The hit.
 * @return Its picture.
 */
NumberPicture numberPicture(const Font *font, const DamageNumber &number)
{
	std::vector<std::pair<std::string, NumberCell>> parts;
	if (number.shield > 0) parts.push_back({ std::to_string(number.shield), CELL_SHIELD }); // (first: the shield is struck first)
	if (number.health > 0) parts.push_back({ std::to_string(number.health), CELL_HEALTH });
	if (number.armor > 0) parts.push_back({ std::to_string(number.armor), CELL_ARMOR });
	if (number.stun > 0) parts.push_back({ std::to_string(number.stun), CELL_STUN });
	if (parts.empty()) parts.push_back({ "0", CELL_NO_HARM });

	const int spacing = font->getSpacing();
	NumberPicture picture;
	picture.height = font->getHeight();
	for (size_t part = 0; part < parts.size(); ++part)
	{
		if (part > 0) picture.width += DamageNumberGap;
		for (size_t character = 0; character < parts[part].first.size(); ++character)
		{
			SurfaceCrop glyph = font->getChar((UCode)(unsigned char)parts[part].first[character]);
			picture.width += glyph.getCrop()->w + (character + 1 < parts[part].first.size() ? spacing : 0);
		}
	}
	picture.width = std::max(1, picture.width);
	picture.cells.assign(picture.width * picture.height, CELL_NONE);
	int penX = 0;
	for (size_t part = 0; part < parts.size(); ++part)
	{
		if (part > 0) penX += DamageNumberGap;
		for (size_t character = 0; character < parts[part].first.size(); ++character)
		{
			SurfaceCrop glyph = font->getChar((UCode)(unsigned char)parts[part].first[character]);
			const Surface *image = glyph.getSurface();
			const SDL_Rect rect = *glyph.getCrop();
			for (int y = 0; y < rect.h && y < picture.height; ++y)
			{
				for (int x = 0; x < rect.w; ++x)
				{
					const Uint8 shade = image->getPixel(rect.x + x, rect.y + y);
					if (shade == 0 || penX + x < 0 || penX + x >= picture.width) continue;
					Uint8 &cell = picture.cells[y * picture.width + penX + x];
					if (shade <= 2) cell = parts[part].second;
					else if (cell == CELL_NONE) cell = CELL_OUTLINE;
				}
			}
			penX += rect.w + (character + 1 < parts[part].first.size() ? spacing : 0);
		}
	}
	return picture;
}

/**
 * How far a number has risen at an age: quickly at first, slowing to a stop at the end.
 * @param age Since the hit (ms).
 * @return Pixels (scale 1).
 */
int riseAt(Uint32 age)
{
	const double share = std::min(1.0, age / (double)DamageNumberLife);
	return (int)(DamageNumberRise * (1.0 - (1.0 - share) * (1.0 - share)) + 0.5);
}

/**
 * The palette colour nearest to a colour, never colour 0 (see-through where a surface is blitted).
 * @param palette The palette.
 * @param red, green, blue The colour (0..255 each).
 * @return The palette index.
 */
Uint8 nearestPaletteColor(const SDL_Color *palette, int red, int green, int blue)
{
	int best = 1, bestDistance = INT_MAX;
	for (int index = 1; index < 256; ++index)
	{
		const int dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
		const int distance = dr * dr + dg * dg + db * db;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = index;
		}
	}
	return (Uint8)best;
}

}

/**
 * Notes a hit's numbers, from now on.
 * @param head The point just over the unit's head, in voxels.
 * @param level The level it's on.
 * @param health The health it lost.
 * @param armor The armour points it lost.
 * @param stun The stun it took.
 * @param shield The points its energy shield lost.
 * @param now The time (SDL ticks).
 */
void DamageNumbers::add(const double head[3], int level, int health, int armor, int stun, int shield, Uint32 now)
{
	DamageNumber number;
	std::copy(head, head + 3, number.head);
	number.level = level;
	number.health = std::max(0, health);
	number.armor = std::max(0, armor);
	number.stun = std::max(0, stun);
	number.shield = std::max(0, shield);
	number.start = now;
	_numbers.push_back(number);
}

/**
 * Drops the numbers that have had their time.
 * @param now The time (SDL ticks).
 * @return True if any are still showing.
 */
bool DamageNumbers::update(Uint32 now)
{
	_numbers.erase(std::remove_if(_numbers.begin(), _numbers.end(), [now](const DamageNumber &number) { return now - number.start >= DamageNumberLife; }), _numbers.end());
	return !_numbers.empty();
}

/**
 * The colours the numbers are drawn in, from a palette: the nearest to a clear red (health),
 * blue (armour), purple (stun), white (an energy shield) and grey (no harm), and to black for
 * their outline.
 * @param palette The palette of the surface drawn on.
 * @return The colours.
 */
DamageNumberColors damageNumberColors(const SDL_Color *palette)
{
	DamageNumberColors colors;
	colors.health = nearestPaletteColor(palette, 255, 40, 40);
	colors.armor = nearestPaletteColor(palette, 70, 130, 255);
	colors.stun = nearestPaletteColor(palette, 190, 80, 230);
	colors.shield = nearestPaletteColor(palette, 255, 255, 255);
	colors.none = nearestPaletteColor(palette, 170, 170, 170);
	colors.outline = nearestPaletteColor(palette, 0, 0, 0);
	return colors;
}

/**
 * Where each number goes now: risen with its age from its spot, then, newest first, each over any
 * newer one it would overlap (by its box, the outline in), so none hides another: a unit's hits
 * close together stack upwards, the newest at the bottom, and the numbers of a blast over units
 * side by side step up over each other; and over the boxes kept clear, from the start if its way up
 * would cross one (a unit struck right under the 3D view's crosshair: its number rises over the
 * crosshair, not into its texts).
 * @param numbers The numbers showing (oldest first).
 * @param spots Where each one's point is on the screen.
 * @param font The small font.
 * @param scale How many times bigger they're drawn.
 * @param now The time (SDL ticks).
 * @param keepClear Boxes no number goes into.
 * @return The row each one's bottom is on (under its box), in the order of `numbers`.
 */
std::vector<int> placeDamageNumbers(const std::vector<DamageNumber> &numbers, const std::vector<DamageNumberSpot> &spots, const Font *font, int scale, Uint32 now,
	const std::vector<DamageNumberBox> &keepClear)
{
	typedef DamageNumberBox Box;
	std::vector<Box> placed = keepClear;
	std::vector<int> bottoms(numbers.size(), 0);
	for (size_t index = numbers.size(); index-- > 0;)
	{
		if (!spots[index].shown)
		{
			continue;
		}
		const NumberPicture picture = numberPicture(font, numbers[index]);
		Box box;
		box.left = spots[index].x - picture.width * scale / 2;
		box.right = box.left + picture.width * scale;
		// A number whose way up would cross a box kept clear, or come close under it, starts over it
		// (rather than jumping over it on the way, or as the view moves a little).
		int start = spots[index].y;
		for (bool moved = true; moved;)
		{
			moved = false;
			for (const Box &clear : keepClear)
			{
				if (box.left < clear.right && clear.left < box.right && start - (DamageNumberRise + picture.height) * scale < clear.bottom + KeepClearMargin * scale && clear.top < start)
				{
					start = clear.top;
					moved = true;
				}
			}
		}
		box.bottom = start - riseAt(now - numbers[index].start) * scale;
		box.top = box.bottom - picture.height * scale;
		// Up over every newer one in the way, until none is (each step clears one, so it ends).
		for (bool moved = true; moved;)
		{
			moved = false;
			for (const Box &other : placed)
			{
				if (box.left < other.right && other.left < box.right && box.top < other.bottom && other.top < box.bottom)
				{
					box.top -= box.bottom - other.top;
					box.bottom = other.top;
					moved = true;
				}
			}
		}
		placed.push_back(box);
		bottoms[index] = box.bottom;
	}
	return bottoms;
}

/**
 * Draws one hit's numbers (see numberPicture): the health lost in red, the armour taken off in
 * blue, the stun taken in purple, or a grey 0, their glyphs' border black; all of it dissolving
 * away over the last of the number's time (the same pixels going first wherever it is).
 * @param surface An 8-bit surface, locked.
 * @param font The game's small font.
 * @param number The hit.
 * @param centreX The column the numbers are centred on.
 * @param bottomY The row under their bottom.
 * @param scale How many times bigger (a pixel of the font is scale x scale).
 * @param now The time (SDL ticks).
 * @param colors The colours (see damageNumberColors).
 * @param depthBuffer The 3D view's depth buffer (one value a pixel of the surface), or null.
 * @param depth How far ahead the number is, with a depth buffer.
 */
void drawDamageNumber(Surface *surface, const Font *font, const DamageNumber &number, int centreX, int bottomY, int scale, Uint32 now,
	const DamageNumberColors &colors, const float *depthBuffer, double depth)
{
	const NumberPicture picture = numberPicture(font, number);
	const Uint8 cellColors[] = { 0, colors.outline, colors.health, colors.armor, colors.stun, colors.shield, colors.none };

	// How much of it shows: all of it, then dissolving away.
	const double share = (now - number.start) / (double)DamageNumberLife;
	const double showing = share < DamageNumberSolidShare ? 1.0 : std::max(0.0, (1.0 - share) / (1.0 - DamageNumberSolidShare));
	const int shownPixels = (int)(showing * 16.0 + 0.5); // of every 4x4

	// Its picture onto the surface, scale times bigger, centred over the point.
	const int width = surface->getWidth(), height = surface->getHeight();
	const int left = centreX - picture.width * scale / 2, top = bottomY - picture.height * scale;
	for (int cellY = 0; cellY < picture.height; ++cellY)
	{
		for (int cellX = 0; cellX < picture.width; ++cellX)
		{
			const Uint8 cell = picture.cells[cellY * picture.width + cellX];
			if (cell == CELL_NONE || DissolveOrder[cellY & 3][cellX & 3] >= shownPixels) continue;
			for (int blockY = 0; blockY < scale; ++blockY)
			{
				const int pixelY = top + cellY * scale + blockY;
				if (pixelY < 0 || pixelY >= height) continue;
				for (int blockX = 0; blockX < scale; ++blockX)
				{
					const int pixelX = left + cellX * scale + blockX;
					if (pixelX < 0 || pixelX >= width) continue;
					if (depthBuffer && depthBuffer[pixelY * width + pixelX] < depth - DamageNumberDepthMargin) continue;
					*surface->getRaw(pixelX, pixelY) = cellColors[cell];
				}
			}
		}
	}
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
