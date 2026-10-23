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
 * FirstPersonViewScreenEffects.cpp: effects on the screen itself.
 *
 * The arc towards where a hit came from, blood splattered over the view, the flames rising up
 * the view while the soldier is on fire, and the wash laid over the finished picture: night
 * vision, the colour washes (hurt, passing out, mind control, breakdowns, turning into a zombie),
 * TV static and red edges.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <SDL.h>
#include "../Engine/Options.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * The soldier we look through was hurt. Starts the red arc on the edge of the view that points
 * towards where the hit came from (see drawHurt), if that's known and not right on top of the
 * soldier, and after a heavy or close hit splatters blood over the view: a handful of blobs
 * near the edges, more and bigger the harder the hit.
 * @param from The point the hit came from (x, y in voxels; only the direction along the
 *             ground counts), or null if unknown.
 * @param amount How bad the hit was, 0 .. 1.
 * @param splatter True to splatter blood over the view as well.
 */
void FirstPersonView::showHurt(const double *from, float amount, bool splatter)
{
	const Uint32 now = SDL_GetTicks();
	// The direction of the hit along the ground: kept only if its source is more than two
	// voxels away (dx, dy: from the eye to the source, in voxels). A hit from an unknown or too
	// close a place puts out any arc still showing.
	_hurtDirectionKnown = false;
	if (from)
	{
		const double dx = from[0] - _eye[0], dy = from[1] - _eye[1];
		if (dx * dx + dy * dy > 4.0)
		{
			_hurtDirection[0] = dx;
			_hurtDirection[1] = dy;
			_hurtDirectionKnown = true;
			_hurtStart = now;
		}
	}
	if (splatter)
	{
		// Blobs of blood near the edges of the view, more the harder the hit. Each blob's centre
		// is at a random angle around the middle of the view, 55% to 95% of the way to the edge,
		// and its radius a share of the view's width (positions are shares of the view's width
		// and height, 0 .. 1). The random numbers come from a linear congruential generator, its
		// top 24 bits made into a number in [0, 1).
		_bloodSplats.clear();
		const int splatCount = 6 + (int)(amount * 10.0f);
		for (int i = 0; i < splatCount; ++i)
		{
			auto nextRandom = [this]() { _bloodSeed = _bloodSeed * 1664525u + 1013904223u; return (_bloodSeed >> 8) / 16777216.0f; };
			const float angle = nextRandom() * 2.0f * (float)Pi, towardsEdge = 0.55f + 0.4f * nextRandom();
			_bloodSplats.push_back({ 0.5f + 0.5f * towardsEdge * std::cos(angle), 0.5f + 0.5f * towardsEdge * std::sin(angle),
				0.015f + 0.04f * nextRandom() * (0.5f + amount) });
		}
		// The blood starts fading from now, from at least a fair share of full strength.
		_bloodStart = now;
		_bloodAmount = std::clamp(amount, 0.4f, 1.0f);
	}
}

/**
 * The soldier we look through is on fire: a sheet of flame rises from the bottom of the view,
 * tall in the corners and low in the middle so you can still see ahead. Heat rises cell by cell,
 * drifting sideways and cooling at random (the old fire effect of Doom on the PlayStation), in
 * blocks of the 320x200 view's pixels; dark red at the tips through orange and yellow to white
 * at the root. Embers drift up out of it, and its light flickers up from below. Over the world
 * (the weapon in hand still in front). A flare-up (catching fire) makes it leap; put out, the
 * last of the heat rises and dies away.
 */
void FirstPersonView::drawBurning()
{
	// Seconds since the last frame (at most a tenth of a second, nothing on the first frame).
	// A flare-up dies down over a little under a second.
	const Uint32 now = SDL_GetTicks();
	const float elapsedSeconds = _flameLastTick ? std::min<Uint32>(100, now - _flameLastTick) / 1000.0f : 0.0f;
	_flameLastTick = now;
	_flameBurst = std::max(0.0f, _flameBurst - elapsedSeconds * 1.2f);
	// Not burning: no flare-up either, and once the last heat and embers are gone, nothing to do.
	if (_burningAmount <= 0.0f)
	{
		_flameBurst = 0.0f;
		if (!_flamesLit && _embers.empty()) return;
	}
	// The fire grid: one cell per pixel of a 320x200 view (roughly: blockSize screen pixels square),
	// across the whole width and up 70% of the height. A new size (the view was resized)
	// starts the grid over, cold.
	const int viewWidth = getWidth(), viewHeight = getHeight();
	const int blockSize = std::max(1, (int)std::lround(viewHeight / 200.0));
	const int gridWidth = (viewWidth + blockSize - 1) / blockSize, gridHeight = (int)(viewHeight * 0.7) / blockSize;
	if (gridWidth != _flameGridWidth || gridHeight != _flameGridHeight)
	{
		_flameGridWidth = gridWidth;
		_flameGridHeight = gridHeight;
		_flameHeat.assign((size_t)gridWidth * gridHeight, 0);
	}
	// Random bits for the fire: a linear congruential generator, its top 24 bits.
	auto nextRandom = [this]() { _flameSeed = _flameSeed * 1664525u + 1013904223u; return _flameSeed >> 8; };
	// The hottest a cell gets; the colour ramp spreads over 0 .. MaxHeat.
	const int MaxHeat = 48;
	const float basePixel = viewHeight / 200.0f; // sizes and speeds as on a 320x200 view
	// How fierce the fire is (a flare-up adds to it), and the flicker of its light (two sine
	// waves of the time, beating against each other).
	const float fierceness = std::min(1.0f, _burningAmount * (1.0f + 1.2f * _flameBurst));
	const float flicker = 0.8f + 0.2f * std::sin(now * 0.031f) * std::sin(now * 0.017f + 1.0f);
	// The light of the flames, from below: the bottom 30% of the picture tinted orange, more
	// the lower (a curve, so it gathers near the bottom), dithered in.
	if (_burningAmount > 0.0f)
	{
		const int glowRows = (int)(viewHeight * 0.3f);
		for (int screenY = viewHeight - glowRows; screenY < viewHeight; ++screenY)
		{
			const float glow = std::pow((screenY - (viewHeight - glowRows)) / (float)glowRows, 1.6f) * 0.7f * fierceness * flicker;
			Uint8 *row = getRaw(0, screenY);
			for (int screenX = 0; screenX < viewWidth; ++screenX)
			{
				if (dither(screenX, screenY) < glow) row[screenX] = _colors.orangeTint[row[screenX]];
			}
		}
	}
	// The flames step 50 times a second: the bottom row is fed (hotter towards the corners),
	// every cell passes its heat up a row and a little downwind (the wind turns now and then),
	// a little cooler; the middle cools faster, so the flames stay low there (and a flare-up
	// cools them less). The wind blows one way or the other, turning every six seconds or so.
	// At most five steps are caught up in one frame.
	const int wind = std::sin(now * 0.0005f) > 0.0f ? 1 : -1;
	_flameStepTimer = std::min(_flameStepTimer + elapsedSeconds, 0.1f);
	while (_flameStepTimer >= 0.02f)
	{
		_flameStepTimer -= 0.02f;
		// The grid is stored row by row, top row first; the bottom row is the fuel.
		Uint8 *heat = _flameHeat.data();
		Uint8 *bottomRow = heat + (size_t)(gridHeight - 1) * gridWidth;
		const float seconds = now / 1000.0f;
		for (int cellX = 0; cellX < gridWidth; ++cellX)
		{
			// Tongues: hot roots wandering along the bottom (two sine waves of the cell's place
			// on a 320x200 view, drifting with time). Hotter towards the sides (towardsEdge: 0 in
			// the middle, 1 at the sides), with a little randomness; cold when not burning.
			const float basePixelX = cellX * blockSize / basePixel;
			const float tongues = 0.5f + 0.5f * std::sin(basePixelX * 0.21f + seconds * 1.7f) * std::sin(basePixelX * 0.083f - seconds * 1.1f + 1.3f);
			const float towardsEdge = std::abs(cellX - gridWidth * 0.5f) / (gridWidth * 0.5f);
			bottomRow[cellX] = _burningAmount > 0.0f
				? (Uint8)(MaxHeat * fierceness * (0.3f + 0.7f * tongues) * (0.65f + 0.35f * towardsEdge) * (0.85f + 0.15f * (nextRandom() & 255) / 255.0f))
				: 0;
		}
		// And the sides burn up, less the higher (a thin wall of fire framing the view): a few
		// columns at each side are kept at least this hot, up to three quarters of the grid's
		// height, licking up and down.
		if (_burningAmount > 0.0f)
		{
			const int sideWidth = std::max(2, gridWidth / 48);
			for (int cellY = 0; cellY < gridHeight - 1; ++cellY)
			{
				const float heightShare = (gridHeight - 1 - cellY) / (float)(gridHeight - 1); // 0 at the bottom .. 1 at the top
				const float lick = 0.5f + 0.5f * std::sin(cellY * blockSize / basePixel * 0.12f + seconds * 2.3f);
				const Uint8 sideHeat = (Uint8)std::max(0.0f, MaxHeat * fierceness * (0.9f - 1.2f * heightShare) * (0.6f + 0.4f * lick));
				for (int inset = 0; inset < sideWidth; ++inset)
				{
					Uint8 &leftCell = heat[cellY * gridWidth + inset], &rightCell = heat[cellY * gridWidth + gridWidth - 1 - inset];
					leftCell = std::max(leftCell, sideHeat);
					rightCell = std::max(rightCell, sideHeat);
				}
			}
		}
		// The heat rises: row by row, from the second row from the top down to the bottom, each
		// cell's heat moves into the row above it (whose own heat has already moved on this
		// step), cooled by 0, 1 or 2. It lands straight above, or one cell to the left or right
		// (a quarter of the time each), and one in eight times a cell further downwind. The
		// cooling: one half the time, plus one more with a chance that's high in the middle and
		// low at the sides (less during a flare-up). Cells without heat leave the cell above
		// them cold.
		bool anyHeat = false;
		for (int cellY = 1; cellY < gridHeight; ++cellY)
		{
			for (int cellX = 0; cellX < gridWidth; ++cellX)
			{
				const Uint8 cellHeat = heat[cellY * gridWidth + cellX];
				if (!cellHeat)
				{
					heat[(cellY - 1) * gridWidth + cellX] = 0;
					continue;
				}
				anyHeat = true;
				// Bits 0-1: sideways jitter; bits 2-4: the wind; bit 4 (shared with the wind): the
				// plain cooling; bits 8-15: the extra cooling.
				const Uint32 randomBits = nextRandom();
				const int jitter = (randomBits & 3) == 0 ? -1 : (randomBits & 3) == 3 ? 1 : 0;
				const int toX = std::clamp(cellX + jitter + (((randomBits >> 2) & 7) == 0 ? wind : 0), 0, gridWidth - 1);
				const float towardsEdge = std::abs(cellX - gridWidth * 0.5f) / (gridWidth * 0.5f);
				const float extraCoolingChance = (0.1f + (1.0f - towardsEdge) * (1.0f - towardsEdge) * 0.55f) * (1.0f - 0.6f * _flameBurst);
				const int cooling = (int)((randomBits >> 4) & 1) + (((randomBits >> 8) & 255) < extraCoolingChance * 255.0f ? 1 : 0);
				heat[(cellY - 1) * gridWidth + toX] = cellHeat > cooling ? (Uint8)(cellHeat - cooling) : 0;
			}
		}
		// The flames stay lit while there's heat left to rise, or while the soldier burns.
		_flamesLit = anyHeat || _burningAmount > 0.0f;
	}
	// Drawn in blocks: white at the root, yellow, orange, dark red, wispy at the tips.
	// The ramp runs from cold to hot: dark red darkened, dark red, orange, yellow, white-hot.
	const Uint8 ramp[5] = { shadeColor(_colors.fire[3], 4), _colors.fire[3], _colors.fire[2], _colors.fire[1], _colors.fire[0] };
	for (int cellY = 0; cellY < gridHeight; ++cellY)
	{
		// The grid's bottom row sits on the bottom of the view.
		const int blockTop = viewHeight - (gridHeight - cellY) * blockSize;
		for (int cellX = 0; cellX < gridWidth; ++cellX)
		{
			const int cellHeat = _flameHeat[cellY * gridWidth + cellX];
			if (!cellHeat) continue;
			const int blockLeft = cellX * blockSize;
			// The faintest heat (below 7) shows only in some cells, fewer the fainter: wisps.
			if (cellHeat < 7 && dither(blockLeft, blockTop) >= cellHeat / 7.0f) continue;
			// The heat as a position on the ramp (0 .. 4): the band it's in, dithered towards
			// the next band by how far into it it is.
			const float rampPosition = cellHeat / (float)MaxHeat * 4.0f;
			const int band = std::min(3, (int)rampPosition);
			const Uint8 color = dither(blockLeft + 1, blockTop + 2) < rampPosition - band ? ramp[band + 1] : ramp[band];
			// Fill the cell's block of screen pixels, clipped to the view.
			for (int pixelY = std::max(0, blockTop); pixelY < std::min(viewHeight, blockTop + blockSize); ++pixelY)
			{
				Uint8 *row = getRaw(0, pixelY);
				for (int pixelX = blockLeft; pixelX < std::min(viewWidth, blockLeft + blockSize); ++pixelX) row[pixelX] = color;
			}
		}
	}
	// Embers drifting up out of it, twinkling. While burning, about 30 a second at full
	// fierceness (the fraction of an ember carries over to the next frame), at most 200 at once.
	_emberDebt += elapsedSeconds * (_burningAmount > 0.0f ? 30.0f * fierceness : 0.0f);
	while (_emberDebt >= 1.0f)
	{
		_emberDebt -= 1.0f;
		if (_embers.size() >= 200) continue;
		// A new ember (screen pixels, per second): across the width, more of them towards the
		// sides (the square root of a random -1 .. 1 pushes them out), low in the view, drifting
		// a little sideways and rising, living 0.8 to 1.8 seconds.
		Ember ember;
		const float across = _particles.random(-1.0f, 1.0f);
		ember.x = viewWidth * 0.5f + (across < 0.0f ? -1.0f : 1.0f) * std::sqrt(std::abs(across)) * viewWidth * 0.5f;
		ember.y = viewHeight - _particles.random(0.05f, 0.3f) * viewHeight;
		ember.vx = _particles.random(-20.0f, 20.0f) * basePixel;
		ember.vy = -_particles.random(50.0f, 100.0f) * basePixel;
		ember.age = 0.0f;
		ember.life = _particles.random(0.8f, 1.8f);
		ember.phase = _particles.random(0.0f, 6.3f);
		_embers.push_back(ember);
	}
	// Move and draw the embers: each a square the size of a fire cell.
	const int emberSize = std::max(1, blockSize);
	for (size_t emberIndex = 0; emberIndex < _embers.size();)
	{
		// An ember that has lived its life goes: the last one takes its place (and is handled
		// next, as the index stays).
		Ember &ember = _embers[emberIndex];
		ember.age += elapsedSeconds;
		if (ember.age >= ember.life)
		{
			ember = _embers.back();
			_embers.pop_back();
			continue;
		}
		// It rises, swaying from side to side.
		ember.x += (ember.vx + std::sin(ember.age * 7.0f + ember.phase) * 25.0f * basePixel) * elapsedSeconds;
		ember.y += ember.vy * elapsedSeconds;
		++emberIndex;
		// Twinkling: it goes dark for a moment several times a second.
		if (std::sin(ember.age * 30.0f + ember.phase) < -0.6f) continue;
		// It cools as it ages: white-hot, then yellow, then orange.
		const Uint8 color = _colors.fire[ember.age < ember.life * 0.5f ? 0 : ember.age < ember.life * 0.8f ? 1 : 2];
		const int emberX = (int)ember.x, emberY = (int)ember.y;
		for (int pixelY = std::max(0, emberY); pixelY < std::min(viewHeight, emberY + emberSize); ++pixelY)
		{
			for (int pixelX = std::max(0, emberX); pixelX < std::min(viewWidth, emberX + emberSize); ++pixelX) *getRaw(pixelX, pixelY) = color;
		}
	}
}

/**
 * Draws what being under fire shows on the screen: a red arc on the edge of the view pointing
 * towards where the last hit came from (it turns with you, fading over 1.6 seconds), and blood
 * on the view after a heavy or close hit (fading over 1.8 seconds); both fade by dithering, so
 * fewer and fewer of their pixels are drawn. See showHurt.
 */
void FirstPersonView::drawHurt()
{
	const Uint32 now = SDL_GetTicks();
	const int viewWidth = getWidth(), viewHeight = getHeight();
	const Uint32 ArcTime = 1600, SplatTime = 1800; // ms
	// The arc: 41 round dots along an ellipse around the middle of the view (44% of the width
	// and 42% of the height out from it), spanning 36 degrees, thickest in the middle.
	if (_hurtDirectionKnown && now - _hurtStart < ArcTime)
	{
		const float fade = 1.0f - (now - _hurtStart) / (float)ArcTime;
		// The direction relative to where we look: ahead is the top of the view, behind the
		// bottom, right the right side (radians, clockwise from ahead).
		const double arcAngle = std::atan2(_hurtDirection[1], _hurtDirection[0]) - _yaw;
		const float centerX = viewWidth * 0.5f, centerY = viewHeight * 0.5f, radiusX = viewWidth * 0.44f, radiusY = viewHeight * 0.42f;
		const float thickness = std::max(3.0f, viewWidth / 110.0f);
		for (int step = -20; step <= 20; ++step)
		{
			// This dot's place on the ellipse (0.9 degrees apart), and its radius, tapering
			// towards the arc's ends.
			const double dotAngle = arcAngle + step * (Pi / 180.0) * 0.9;
			const float dotX = centerX + (float)std::sin(dotAngle) * radiusX, dotY = centerY - (float)std::cos(dotAngle) * radiusY;
			const float taper = 1.0f - std::abs(step) / 21.0f;
			const int dotRadius = (int)std::ceil(thickness * (0.4f + 0.6f * taper));
			// Fill the dot (screen pixels, clipped to the view), dithered by the fade; the ends
			// fade out sooner.
			for (int y = (int)dotY - dotRadius; y <= (int)dotY + dotRadius; ++y)
			{
				for (int x = (int)dotX - dotRadius; x <= (int)dotX + dotRadius; ++x)
				{
					if (x < 0 || y < 0 || x >= viewWidth || y >= viewHeight
						|| (x - dotX) * (x - dotX) + (y - dotY) * (y - dotY) > dotRadius * dotRadius) continue;
					if (dither(x, y) < fade * (0.35f + 0.65f * taper)) *getRaw(x, y) = _colors.blood[1];
				}
			}
		}
	}
	// The blood: each splat a round blob, dark inside and bright at the rim. As it fades, its
	// rim goes first (the dithering threshold falls off towards the edge).
	if (!_bloodSplats.empty() && now - _bloodStart < SplatTime)
	{
		const float fade = _bloodAmount * (1.0f - (now - _bloodStart) / (float)SplatTime);
		for (const Splat &splat : _bloodSplats)
		{
			// The splat in screen pixels (stored as shares of the view; its radius of the width).
			const float splatX = splat.x * viewWidth, splatY = splat.y * viewHeight, splatRadius = splat.r * viewWidth;
			for (int y = std::max(0, (int)(splatY - splatRadius)); y < std::min(viewHeight, (int)(splatY + splatRadius) + 1); ++y)
			{
				for (int x = std::max(0, (int)(splatX - splatRadius)); x < std::min(viewWidth, (int)(splatX + splatRadius) + 1); ++x)
				{
					// How far out from the splat's centre this pixel is: 0 centre .. 1 rim.
					const float distanceShare = std::sqrt((x - splatX) * (x - splatX) + (y - splatY) * (y - splatY)) / splatRadius;
					if (distanceShare > 1.0f || dither(x + 1, y + 2) >= fade * (1.2f - distanceShare)) continue;
					*getRaw(x, y) = distanceShare < 0.6f ? _colors.blood[0] : _colors.blood[1];
				}
			}
		}
	}
}

/**
 * Washes the finished picture, pixel by pixel, with whatever applies right now. Night vision
 * recolours everything into one colour group (keeping each shade, like the map's night
 * vision). A colour wash (red: hurt or killed, dark blue: unconscious, orange: mind control,
 * white: a breakdown, sickly green: turning into a zombie; see setTint) dithers in over that
 * share of the pixels, some of them washed red instead if asked. TV static (a helmet camera
 * losing its signal) replaces a share of the pixels with random greys. And the screen edges
 * redden for a soldier badly hurt (the vignette). Called from draw, after the world and
 * everything in it are drawn and before the flames, the hurt arc and the markers.
 * Every pixel is washed on its own, so the work is shared out between all the view's threads,
 * a band of rows each (see washBands).
 */
void FirstPersonView::applyScreenWash()
{
	// Nothing to wash with: leave the picture alone (and skip the loop over every pixel).
	if (!(_nightVision || _tintAmount > 0.0f || _vignette > 0.0f || _staticAmount > 0.0f))
	{
		return;
	}
	// The static's pattern changes every frame: each row starts its own random numbers from this.
	_staticSeed = _staticSeed * 1664525u + 1013904223u;
	runOnAllThreads(&FirstPersonView::washBands);
}

/**
 * The screen wash (see applyScreenWash) over bands of the view's rows, taken one at a time from
 * the shared counter until none are left: a job for all the view's threads (see runOnAllThreads).
 */
void FirstPersonView::washBands()
{
	// The middle of the view and the distance from it to a corner (for the vignette), the night
	// vision's colour group (its first palette index), and the colour table of the wash asked for.
	const int viewWidth = getWidth(), viewHeight = getHeight();
	const float centerX = viewWidth * 0.5f, centerY = viewHeight * 0.5f, cornerDistance = std::sqrt(centerX * centerX + centerY * centerY);
	const Uint8 nightGroup = _nightVision ? (Uint8)((_nightVision - 1) * 16) : 0;
	const Uint8 *wash = _tintKind == 1 ? _colors.blueTint : _tintKind == 2 ? _colors.orangeTint : _tintKind == 3 ? _colors.whiteTint
		: _tintKind == 4 ? _colors.greenTint : _colors.redTint;
	// Every pixel of the band (x, y: screen pixels), each effect in turn.
	const int bandCount = (viewHeight + BandRows - 1) / BandRows;
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		for (int y = band * BandRows; y < std::min(viewHeight, (band + 1) * BandRows); ++y)
		{
			Uint8 *row = getRaw(0, y);
			Uint32 staticSeed = _staticSeed ^ ((Uint32)y * 2654435761u); // the row's own random numbers (see below)
			for (int x = 0; x < viewWidth; ++x)
			{
				// Night vision: the pixel's shade (the low four bits) moved into the night vision
				// group. Index 0 (transparent) stays as it is.
				if (_nightVision && row[x]) row[x] = nightGroup | (row[x] & 15);
				// The colour wash, on the share of pixels the dither pattern lets through; of those,
				// a share (by a second, shifted dither) is washed red instead.
				if (_tintAmount > 0.0f && dither(x, y) < _tintAmount)
					row[x] = (_tintRedShare > 0.0f && dither(y + 1, x + 2) < _tintRedShare ? _colors.redTint : wash)[row[x]];
				// TV static: a linear congruential generator, a step per pixel (each row starts its
				// own from the frame's seed and the row's number, so any thread can wash any row);
				// bits 8-15 decide whether the pixel is static, the top two bits which of four greys.
				if (_staticAmount > 0.0f)
				{
					staticSeed = staticSeed * 1664525u + 1013904223u;
					if (((staticSeed >> 8) & 255) < _staticAmount * 256.0f) row[x] = _colors.grey[staticSeed >> 30];
				}
				// The vignette: reddened, dithered in, from 55% of the way to the corners outwards,
				// fully at the corners at full strength.
				if (_vignette > 0.0f)
				{
					const float distanceShare = std::sqrt((x - centerX) * (x - centerX) + (y - centerY) * (y - centerY)) / cornerDistance;
					if (smoothstep(0.55f, 1.0f, distanceShare) * _vignette > dither(x + 3, y + 1)) row[x] = _colors.redTint[row[x]];
				}
			}
		}
	}
}

/**
 * A helmet camera's digital feed with a bad signal (see setSignalGlitches), moved on by a frame:
 * now and then, for a moment, a glitch. Most often a few patches of the picture fall into coarse
 * blocks (sometimes with a square or two of the decoder's green), less often only green squares,
 * least often the picture freezes. Each lasts a fifth of a second or so (a freeze up to half a
 * second), the next comes some seconds later: enough to tell a camera's feed, never enough to
 * get in the way. On the hardest missions (level 2, see RuleFirstPerson::getSignalLevel: Mars,
 * the aliens' bases, a base defence), and on any mission while the soldier watched stands inside
 * a UFO, the signal is bad: a glitch every second or two, half as long again, in more and bigger
 * patches, and now and then torn (bands of the picture slid sideways). Never with Muzzle flashes off (the setting for those bothered by flashing
 * light). A camera shown again after a pause starts afresh: no glitch for a few seconds, and a
 * freeze never holds an old picture (it also ends when the camera moves to another soldier).
 * Called at the start of every draw.
 * @return True while the picture is frozen (draw then keeps the last one).
 */
bool FirstPersonView::updateSignalGlitches()
{
	const Uint32 now = SDL_GetTicks();
	auto random = [this](int low, int high)
	{
		_glitchSeed = _glitchSeed * 1664525u + 1013904223u;
		return low + (int)((_glitchSeed >> 8) % (Uint32)(high - low + 1));
	};
	const bool shownLately = _lastDrawTick && now - _lastDrawTick < 250;
	_lastDrawTick = now;
	// A bad signal (level 2, the hardest missions; or the soldier watched stands inside a UFO,
	// amid its machinery) glitches every second or two, longer and in more and bigger patches;
	// the usual one every few seconds, briefly.
	const Tile *watchedTile = _cameraUnit ? _cameraUnit->getTile() : nullptr;
	const int watchedIndex = watchedTile ? _save->getTileIndex(watchedTile->getPosition()) : -1;
	const bool insideUfo = watchedIndex >= 0 && watchedIndex < (int)_ufoPhaseOfTile.size() && (_ufoPhaseOfTile[watchedIndex] & UfoTileBit);
	const bool bad = _signalGlitches >= 2 || (_signalGlitches == 1 && insideUfo);
	if (!Options::fpsMuzzleFlash || !shownLately)
	{
		_glitchKind = GLITCH_NONE;
		_glitchNextAt = now + (bad ? random(500, 1500) : random(2000, 6000));
		return false;
	}

	// The glitch going on ends in its time (a freeze also when the camera moves on).
	if (_glitchKind != GLITCH_NONE && (now >= _glitchUntil || (_glitchKind == GLITCH_FREEZE && _cameraUnit != _glitchFrozenUnit)))
	{
		_glitchKind = GLITCH_NONE;
	}

	// The next one: which, how long, and the one after it.
	if (_glitchKind == GLITCH_NONE && now >= _glitchNextAt)
	{
		// Which: blocks, green squares or a freeze; a bad signal also tears.
		const int roll = random(0, 99);
		if (bad) _glitchKind = roll < 40 ? GLITCH_BLOCKS : roll < 60 ? GLITCH_GREEN : roll < 85 ? GLITCH_TEAR : GLITCH_FREEZE;
		else _glitchKind = roll < 55 ? GLITCH_BLOCKS : roll < 80 ? GLITCH_GREEN : GLITCH_FREEZE;
		const int length = _glitchKind == GLITCH_BLOCKS ? random(120, 360) : _glitchKind == GLITCH_GREEN ? random(60, 200) : _glitchKind == GLITCH_TEAR ? random(80, 220) : random(200, 550);
		_glitchUntil = now + (bad ? length * 3 / 2 : length);
		_glitchNextAt = _glitchUntil + (bad ? random(600, 2500) : random(3500, 9000));
		_glitchFrozenUnit = _cameraUnit;
		// Its patches, in blocks: one to three small rectangles of coarse blocks (a third of the
		// time with a green square), or one to four green squares in a row or two. A bad signal:
		// two to five bigger rectangles with a green square, or up to three groups of squares.
		_glitchPatches.clear();
		const int blocksAcross = std::max(1, getWidth() / glitchBlock()), blocksDown = std::max(1, getHeight() / glitchBlock());
		if (_glitchKind == GLITCH_BLOCKS)
		{
			for (int patch = bad ? random(2, 5) : random(1, 3); patch > 0; --patch)
			{
				const int width = std::min(blocksAcross, bad ? random(3, 9) : random(2, 6)), height = std::min(blocksDown, bad ? random(1, 4) : random(1, 3));
				_glitchPatches.push_back({ random(0, blocksAcross - width), random(0, blocksDown - height), width, height, false });
			}
			if (bad || random(0, 2) == 0)
			{
				_glitchPatches.push_back({ random(0, blocksAcross - 1), random(0, blocksDown - 1), 1, 1, true });
			}
		}
		else if (_glitchKind == GLITCH_TEAR)
		{
			// One to three bands across the picture, a twelfth to a quarter of its height each,
			// slid a tenth to a quarter of its width to either side.
			const int viewWidth = getWidth(), viewHeight = getHeight();
			for (int band = random(1, 3); band > 0; --band)
			{
				const int rows = random(std::max(1, viewHeight / 12), std::max(1, viewHeight / 4));
				const int slide = random(std::max(1, viewWidth / 10), std::max(1, viewWidth / 4)) * (random(0, 1) ? 1 : -1);
				_glitchPatches.push_back({ slide, random(0, std::max(0, viewHeight - rows)), 0, rows, false });
			}
		}
		else if (_glitchKind == GLITCH_GREEN)
		{
			for (int group = bad ? random(1, 3) : 1; group > 0; --group)
			{
				const int width = std::min(blocksAcross, random(1, 4)), height = std::min(blocksDown, random(1, 2));
				_glitchPatches.push_back({ random(0, blocksAcross - width), random(0, blocksDown - height), width, height, true });
			}
		}
	}
	return _glitchKind == GLITCH_FREEZE;
}

/**
 * Draws the bad signal's glitch going on (see updateSignalGlitches) over the finished picture:
 * each block of its patches one flat colour, the pixel in its middle (every other block the one
 * a block to its left, smeared on as a broken motion vector leaves it), or the decoder's green,
 * bright and dark by turns; or a tear, bands of the picture slid sideways. Called from draw
 * after the screen wash.
 */
void FirstPersonView::applySignalGlitches()
{
	const int block = glitchBlock(), viewWidth = getWidth(), viewHeight = getHeight();
	// A tear: each band's rows slid sideways, what slides off one side coming in at the other,
	// and its top edge a bright seam (each pixel three shades brighter in its colour group,
	// stopping at the group's brightest; group 0's is index 1, index 0 being empty black).
	if (_glitchKind == GLITCH_TEAR)
	{
		for (const GlitchPatch &band : _glitchPatches)
		{
			const int slide = ((band.x % viewWidth) + viewWidth) % viewWidth;
			for (int y = band.y; y < std::min(viewHeight, band.y + band.height); ++y)
			{
				Uint8 *row = getRaw(0, y);
				std::rotate(row, row + (viewWidth - slide) % viewWidth, row + viewWidth);
				if (y != band.y) continue;
				for (int x = 0; x < viewWidth; ++x)
				{
					const int brightest = (row[x] & 0xF0) == 0 ? 1 : 0;
					row[x] = (Uint8)((row[x] & 0xF0) | std::max(brightest, (row[x] & 15) - 3));
				}
			}
		}
		return;
	}
	if (_glitchKind != GLITCH_BLOCKS && _glitchKind != GLITCH_GREEN)
	{
		return;
	}
	for (const GlitchPatch &patch : _glitchPatches)
	{
		for (int blockY = patch.y; blockY < patch.y + patch.height; ++blockY)
		{
			for (int blockX = patch.x; blockX < patch.x + patch.width; ++blockX)
			{
				const int left = blockX * block, top = blockY * block;
				if (left >= viewWidth || top >= viewHeight) continue;
				const int sampleX = std::clamp(left + block / 2 - ((blockX & 1) ? block : 0), 0, viewWidth - 1);
				const int sampleY = std::min(viewHeight - 1, top + block / 2);
				const Uint8 fill = patch.green ? _colors.glitchGreen[(blockX + blockY) & 1] : *getRaw(sampleX, sampleY);
				for (int y = top; y < std::min(viewHeight, top + block); ++y)
				{
					std::fill(getRaw(left, y), getRaw(left, y) + (std::min(viewWidth, left + block) - left), fill);
				}
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
