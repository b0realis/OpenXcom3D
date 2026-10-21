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
 * FirstPersonViewShared.h: the constants and small helpers that the FirstPersonView*.cpp files
 * share (proportions of the voxel world, timings, dithering, colour shading, noise, the tiny
 * fonts). Only those files include it (everything here is private to each of them).
 */
#include <algorithm>
#include <cmath>
#include <limits>
#include <SDL.h>
#include "../Battlescape/Position.h"

namespace OpenXcom
{

namespace
{

// ---- Numbers ----

const double Pi = 3.14159265358979323846;
/// The size of one tile in voxels on each axis: 16 across (x, y), 24 up (z).
const float CellSize[3] = { (float)Position::TileXY, (float)Position::TileXY, (float)Position::TileZ };
const float Infinity = std::numeric_limits<float>::infinity();

// ---- Sprites ----

/// Unit and item sprites are 32x40 isometric frames; this buffer fits a 2x2 unit.
const int SpriteBufferSize = 64;
/// Sprite upscaling (an option): how much bigger the smooth copy is (xBRZ), and from how many
/// screen pixels per sprite pixel on it is used (further away the original looks as good).
const int UpscaleFactor = 4;
const double UpscaleFrom = 1.6;
/// A thrown item flies as its voxel model (its inventory picture given volume): this many voxels
/// per picture pixel (a grenade's picture, about 12 pixels, is a third of a tile across),
/// turning end over end this many times a second.
const double ThrownItemScale = 0.35, ThrownItemSpin = 1.6;

// ---- Timings and limits ----

/// How long doors take to open in the view, in milliseconds, so you see them move (the game's
/// own doors open at once): a door swinging on its hinge, a UFO door sliding aside.
const Uint32 DoorSwingTime = 500, UfoDoorTime = 600;
/// Animated terrain (water, machinery, the lights and lit floors of UFOs) steps to its next
/// frame every CalmFrameTime milliseconds in the view, cross-fading over the whole step. The
/// game steps every 100 ms, which up close made whole lit panels blink five to ten times a
/// second: a photosensitivity hazard (WCAG 2.3 allows at most three flashes a second). At this
/// pace a two-frame blink is a soft pulse of about 1.6 Hz.
const Uint32 CalmFrameTime = 320;
/// The marks on a UFO's tiles in FirstPersonView::_ufoPhaseOfTile, above the pulse phase (bits
/// 0-3): a tile of the UFO, and one of its outer skin (beside a tile that isn't the UFO's).
const Uint8 UfoTileBit = 0x80, UfoSkinBit = 0x40;
/// Tactical flares: how long they burn (seconds), how far their light reaches (voxels), and how
/// many can burn at once.
const float FlareLife = 60.0f, FlareReach = 150.0f;
const size_t FlareMax = 8;
/// Pixel size Auto (the option at 0): views up to this size get full detail, bigger ones blocks
/// of 2. A 1920x1080 screen at the default battlescape scale makes a 960x540 view, which ran at
/// 40-51 FPS in a busy town; in blocks of 2 it holds 60 and looks almost the same.
const int AutoPixelMaxWidth = 640, AutoPixelMaxHeight = 400;

// ---- Proportions of the voxel world ----

/// Top of the ground-level floor, in voxels (floors are solid in voxel layers 0-1).
const float GroundTop = 2.0f;
/// The discreet map edge (setting): a small red dot on the floor along the edge every this many
/// voxels (16 to a tile), this far inside the edge, this big across (voxels).
const float EdgeDotSpacing = 16.0f, EdgeDotInset = 3.0f, EdgeDotSize = 2.5f;
/// X-COM's voxels aren't cubes. The isometric art is drawn in 2:1 dimetric with one pixel per
/// voxel up, where a true projection of cubes would draw 1.22 pixels: one voxel up is sqrt(2/3)
/// of one voxel across. The camera works in true proportions; rays walk the voxel grid.
const double ZScale = 0.81649658092772603;
const float ZScaleF = (float)ZScale;
/// An isometric sprite pixel across is 1/sqrt(2) of a voxel (32 pixels span a tile's diagonal).
const double SpritePixelX = 0.70710678118654752;
/// Sprites stand like cards facing the camera, leaning with the view's pitch; looking down
/// further than this many degrees, a card leans only this far, so it stays above the floor
/// instead of lying in it (see drawSprite).
const double CardTiltMax = 45.0;
/// Looking up, a card leans only this far (its top comes towards the camera): a unit right
/// beside the soldier leaning in by much more lurched at the camera.
const double CardTiltUpMax = 10.0;
/// See-through smoke over a unit the soldier sees veils it by at most this many see-through steps
/// (of FirstPersonView::SeeThroughLevels) in all, however much smoke lies in front: it stays clear.
const int UnitVeilMax = 5;
/// The see-through smoke keeps this share of its colours (the rest turned to grey; see softSmokeFrame).
const float SmokeColorKept = 0.35f;
/// A lying sprite's rows below its anchor lie on the floor, each this many voxels nearer the
/// camera: one isometric pixel down is sqrt(2) voxels along the floor towards the viewer.
const double LyingRowVoxels = 1.41421356237309505;
/// Things on the ground are drawn standing on the floor slab the view builds (two voxels thick),
/// raised out of whatever solid they are in by at most this many voxels (see standingHeight).
const int StandRaiseMax = 4;

// ---- Colour helpers ----

/// Ordered dithering (a 4x4 Bayer matrix): a threshold per pixel, used to turn fractional
/// shades, washes and gradients into the 8-bit palette's whole colours without banding.
const float Bayer[4][4] = {
	{ 0 / 16.0f, 8 / 16.0f, 2 / 16.0f, 10 / 16.0f },
	{ 12 / 16.0f, 4 / 16.0f, 14 / 16.0f, 6 / 16.0f },
	{ 3 / 16.0f, 11 / 16.0f, 1 / 16.0f, 9 / 16.0f },
	{ 15 / 16.0f, 7 / 16.0f, 13 / 16.0f, 5 / 16.0f } };

/**
 * The dithering threshold (0 .. 15/16) for a pixel: something covering a fraction f of the
 * screen is drawn on the pixels where dither(x, y) < f.
 */
inline float dither(int x, int y) { return Bayer[y & 3][x & 3]; }

/**
 * Darkens a palette colour within its 16-colour group, like the game's StandardShade (each
 * group runs from bright to dark): anything that would spill into the next group becomes black.
 * @param color The palette index.
 * @param shade How many steps darker (0 or less: unchanged).
 */
inline Uint8 shadeColor(Uint8 color, int shade)
{
	if (shade <= 0)
	{
		return color;
	}
	const int shaded = color + shade;
	if (shaded > 255 || ((shaded ^ color) & 0xF0))
	{
		return 0x0F;
	}
	return (Uint8)shaded;
}

/**
 * A smooth step from 0 (at or below edgeLow) to 1 (at or above edgeHigh), easing in and out.
 */
inline float smoothstep(float edgeLow, float edgeHigh, float value)
{
	const float progress = std::clamp((value - edgeLow) / (edgeHigh - edgeLow), 0.0f, 1.0f);
	return progress * progress * (3.0f - 2.0f * progress);
}

// ---- Noise (clouds, flicker, scattering) ----

/**
 * A well-mixed hash of two integers: the same pair always gives the same pseudo-random bits.
 */
inline Uint32 hash2(int x, int y)
{
	Uint32 mixed = (Uint32)x * 374761393u + (Uint32)y * 668265263u;
	mixed = (mixed ^ (mixed >> 13)) * 1274126177u;
	return mixed ^ (mixed >> 16);
}

/**
 * Smooth value noise in [0, 1): a random value at every whole point of the plane, blended
 * smoothly in between.
 */
inline float valueNoise(float x, float y)
{
	// The grid square the point is in, and where in it (eased, so the blend has no creases).
	const int cornerX = (int)std::floor(x), cornerY = (int)std::floor(y);
	const float insideX = x - cornerX, insideY = y - cornerY;
	const float easeX = insideX * insideX * (3 - 2 * insideX), easeY = insideY * insideY * (3 - 2 * insideY);
	// The random values at its four corners, blended along x, then along y.
	auto cornerValue = [](int gridX, int gridY) { return (hash2(gridX, gridY) & 0xFFFF) / 65536.0f; };
	const float topLeft = cornerValue(cornerX, cornerY), topRight = cornerValue(cornerX + 1, cornerY);
	const float bottomLeft = cornerValue(cornerX, cornerY + 1), bottomRight = cornerValue(cornerX + 1, cornerY + 1);
	return (topLeft + (topRight - topLeft) * easeX) + ((bottomLeft + (bottomRight - bottomLeft) * easeX) - (topLeft + (topRight - topLeft) * easeX)) * easeY;
}

/**
 * Cloud-like noise: three layers of value noise, each finer and fainter than the last.
 */
inline float cloudNoise(float x, float y)
{
	return valueNoise(x, y) * 0.55f + valueNoise(x * 2.1f + 17.0f, y * 2.1f) * 0.3f + valueNoise(x * 4.3f, y * 4.3f + 31.0f) * 0.15f;
}

// ---- Tiny fonts, drawn into the world ----

/// 3x5 glyphs for the text on the yellow tape around the map border: rows top to bottom, each
/// row three bits with bit 2 the left column.
const Uint8 TapeFont[][5] = {
	{ 5, 5, 2, 5, 5 }, // X
	{ 0, 0, 7, 0, 0 }, // -
	{ 7, 4, 4, 4, 7 }, // C
	{ 7, 5, 5, 5, 7 }, // O
	{ 5, 7, 7, 5, 5 }, // M
	{ 0, 0, 0, 0, 0 }, // space
};
/// The tape's text, repeated along it, as glyphs of TapeFont: "X-COM  ".
const int TapeText[] = { 0, 1, 2, 3, 4, 5, 5 };
/// 3x5 digits for the numbered markers over the visible enemies, same layout as TapeFont.
const Uint8 DigitFont[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 3, 1, 7 }, { 5, 5, 7, 1, 1 },
	{ 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 } };
const int TapeTextLength = sizeof(TapeText) / sizeof(TapeText[0]);

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
