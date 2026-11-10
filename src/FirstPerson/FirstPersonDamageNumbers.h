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
 * FirstPersonDamageNumbers.h: setting "Damage numbers": a hit's damage rising over the unit hit,
 * in the 3D view and on the isometric map alike (see FirstPersonDamageNumbers.cpp).
 */
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

class Font;
class Surface;

namespace FirstPerson
{

/// How long a hit's numbers show, rising and fading (ms).
const Uint32 DamageNumberLife = 1500;

/// One hit's numbers over the unit hit.
struct DamageNumber
{
	double head[3];   // the point just over the unit's head as it was hit, in voxels: the numbers rise from there
	int level;        // the level it was on (the map shows the levels up to the one looked at)
	int health;       // the health it lost (red)
	int armor;        // the armour points it took off the side struck (blue)
	int stun;         // the stun it took (purple); none of the three: a grey 0
	Uint32 start;     // when it was hit (SDL ticks)
};

/// The hits whose numbers show now, oldest first.
class DamageNumbers
{
public:
	/// Notes a hit's numbers (see DamageNumber).
	void add(const double head[3], int level, int health, int armor, int stun, Uint32 now);
	/// Drops the numbers that have had their time; true if any are left.
	bool update(Uint32 now);
	/// The numbers showing.
	const std::vector<DamageNumber> &numbers() const { return _numbers; }
private:
	std::vector<DamageNumber> _numbers;
};

/// The palette colours the numbers are drawn in: the nearest to a clear red, blue, purple and grey,
/// and black for their outline.
struct DamageNumberColors { Uint8 health, armor, stun, none, outline; };
DamageNumberColors damageNumberColors(const SDL_Color *palette);

/// Where a number's point is on the screen: the column it's centred on and the row it rises from
/// (shown false: behind the camera or off the map's levels, it takes no room).
struct DamageNumberSpot { int x, y; bool shown; };

/// A box on the screen (columns left to right - 1, rows top to bottom - 1).
struct DamageNumberBox { int left, right, top, bottom; };

/// The row each number's bottom is on now, in the order of `numbers`: risen with its age from its
/// spot, and over any newer number it would overlap, so each stays readable (a unit's hits stack
/// upwards, the newest at the bottom; a blast's over units side by side step up), and over the boxes
/// kept clear (the 3D view's crosshair and the texts round it). `scale` times bigger.
std::vector<int> placeDamageNumbers(const std::vector<DamageNumber> &numbers, const std::vector<DamageNumberSpot> &spots, const Font *font, int scale, Uint32 now,
	const std::vector<DamageNumberBox> &keepClear);

/// Draws one hit's numbers in the game's small font on an 8-bit surface (locked), outlined in
/// black, centred on column `centreX`, ending above row `bottomY`, `scale` times bigger, fading out
/// (dissolving) over the last of their time. With a depth buffer (the 3D view's, one value a pixel
/// of the surface), a pixel is drawn only where nothing nearer than `depth` shows (walls and units
/// in front hide them, as anything in the world).
void drawDamageNumber(Surface *surface, const Font *font, const DamageNumber &number, int centreX, int bottomY, int scale, Uint32 now,
	const DamageNumberColors &colors, const float *depthBuffer = nullptr, double depth = 0.0);

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
