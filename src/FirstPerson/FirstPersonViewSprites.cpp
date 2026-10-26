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
 * FirstPersonViewSprites.cpp: units and items standing in the 3D world.
 *
 * The game's own unit and item sprites, drawn where they stand and turned to show the side that
 * faces the camera, easing between the game's steps, flinching when hit, with soft shadows;
 * smoke and fire on tiles; and the numbered markers over the visible enemies.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <SDL.h>
#include "FirstPersonApi.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Battlescape/ItemSprite.h"
#include "../Battlescape/Position.h"
#include "../Battlescape/UnitSprite.h"
#include "FirstPersonParticles.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * Projects a point in the world onto the view: where on the screen it is, and how far ahead
 * of the camera. The camera's axes are in true proportions, so the point's height is first
 * scaled from voxels to true proportions (see ZScale).
 * @param world The point, in voxels.
 * @param screenX Out: its pixel column on the view (fractional, may be off the view).
 * @param screenY Out: its pixel row on the view (fractional, may be off the view).
 * @param depth Out: its distance ahead of the camera along the view direction, the same
 *              measure as the depth buffer.
 * @return False if it's behind the camera, or closer than 4 (then nothing is set).
 */
bool FirstPersonView::projectToScreen(const double world[3], double &screenX, double &screenY, double &depth) const
{
	// The point relative to the eye, in the camera's frame: ahead, to the right and up.
	double relative[3], ahead = 0.0, toRight = 0.0, upward = 0.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		relative[axis] = (world[axis] - _eye[axis]) * (axis == 2 ? ZScale : 1.0);
		ahead += relative[axis] * _forward[axis];
		toRight += relative[axis] * _right[axis];
		upward += relative[axis] * _up[axis];
	}
	if (ahead < 4.0)
	{
		return false;
	}
	// Perspective: the focal length is how many pixels one unit across is at a distance of one.
	const double focalLength = (getWidth() / 2.0) / _halfFovTangent;
	screenX = getWidth() / 2.0 + toRight * focalLength / ahead;
	screenY = getHeight() / 2.0 - upward * focalLength / ahead;
	depth = ahead;
	return true;
}

/**
 * Draws a sprite standing in the world: its anchor pixel at a point in voxels, at the size the
 * isometric map draws it (a sprite pixel is 1/sqrt(2) of a voxel across and one voxel up; times
 * scale), depth-tested against what's already drawn (the ray caster's depth
 * buffer and the sprites before it), so walls and nearer things hide it. Right in front of the
 * camera it fades out, dithered, rather than filling the screen with huge pixels.
 *
 * The sprite is a card standing at the anchor, turned to face the camera. It leans back as the
 * view looks down (and forward as it looks up), so it faces the camera square on, but only up to
 * CardTiltMax degrees: looking further down, the card stays that upright, its top coming
 * towards the camera, and a soldier or a corpse seen from above stays above the floor instead
 * of lying in it (and vanishing). Looking up it leans only CardTiltUpMax degrees: a unit right
 * beside the soldier, leaning in further, lurched at the camera. Within those pitches the card
 * is square to the view, which is simply the sprite scaled at the anchor's distance.
 * A sprite of something lying on the floor (SpritePose::Lying: items, corpses) stands only
 * above the anchor: its rows below the anchor, where the isometric art draws the front half of
 * the floor's diamond, lie flat on the floor in front of the anchor (sqrt(2) voxels nearer the
 * camera a row), half a voxel above it, so they show on the floor rather than under it. Seen
 * from below that floor (a soldier downstairs looking up), they aren't drawn at all: the top of
 * a floor can't be seen from under it, and on the card they hung down through the ceiling.
 * Each screen row is worked out on its own: where its ray meets the card (or the floor), which
 * sprite row that is, how far away, and so how wide the sprite's pixels are along it. With the
 * view tilted by a lean (setRoll) the rows no longer run level, and each pixel is worked out on
 * its own instead; the card stays upright in the world, so it tilts on the screen with the rest.
 * A see-through sprite (the smoke) is laid over what's there by each pixel's opacity, in the
 * see-through steps, faded rather than dithered up close, and leaves the depth as it was (it
 * hides nothing drawn after it, so it is drawn last, farthest first). Over a unit drawn this
 * frame it stays thin (see _unitVeil): what the soldier sees stays clear.
 * @param draw The sprite, where it stands and how (see SpriteDraw): the sprite (palette colours,
 *        0 transparent); its anchor (the sprite pixel standing at the point); the point, in
 *        voxels; the depth bias (how much nearer than the point the sprite counts for the depth
 *        test, so it isn't hidden by the floor it stands on); the shade; the scale (world size
 *        per sprite pixel); how tall the card's rows are (squashed to fit under a floor); the
 *        pose (standing, all of it on the card, or lying, the rows below the anchor on the
 *        floor); how opaque each pixel of a see-through one is (or null: solid); and the unit it
 *        pictures (or null).
 * @param bandTop, bandBottom Only the screen rows from bandTop up to (not including) bandBottom
 *        are drawn: a band of the view, when the drawing is shared out between threads (each
 *        pixel comes out the same as when the sprite is drawn whole).
 */
void FirstPersonView::drawSprite(const SpriteDraw &draw, int bandTop, int bandBottom)
{
	const Surface *sprite = draw.sprite;
	const int anchorX = draw.anchorX, anchorY = draw.anchorY, shade = draw.shade;
	const double *world = draw.world;
	const double depthBias = draw.depthBias, scale = draw.scale;
	const SpritePose pose = draw.pose;
	const SeeThroughSprite *seeThroughSprite = draw.seeThrough;
	// Where the anchor lands on the view, and how far away it is.
	double anchorScreenX, anchorScreenY, viewDepth;
	if (!sprite || !projectToScreen(world, anchorScreenX, anchorScreenY, viewDepth))
	{
		return;
	}
	// Right in front of the camera a sprite is a wall of huge pixels: fade it out (dithered),
	// but not a unit on the next tile (melee range), which stays solid. A see-through sprite
	// fades out instead (its opacity times this).
	const float opacity = (float)std::clamp((viewDepth - 4.0) / 6.0, 0.0, 1.0);
	const float seeThroughOpacity = seeThroughSprite ? seeThroughSprite->opacity * opacity : 1.0f;
	if (opacity <= 0.0f || seeThroughOpacity * SeeThroughLevels < 0.5f)
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const int spriteWidth = sprite->getWidth(), spriteHeight = sprite->getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent;
	auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

	// The card, in the camera's frame (true proportions, as projectToScreen works): the anchor
	// from the eye; the way up the card, upright but leaning with the view's pitch up to
	// CardTiltMax looking down and CardTiltUpMax looking up; and the way the card faces (the
	// view axis, while the pitch is within that). A card standing steeply above the eye (its
	// feet higher than CardTiltUpMax up: a unit on a floor above, a flyer) leans towards the eye
	// by how much steeper it is, up to CardTiltSteepMax, so a unit straight overhead faces down
	// at the soldier, not edge on. (Units on the soldier's own level have their feet below the
	// eye: unchanged.)
	const double level[2] = { std::cos(_yaw), std::sin(_yaw) }; // straight ahead, along the ground
	const double anchor[3] = { world[0] - _eye[0], world[1] - _eye[1], (world[2] - _eye[2]) * ZScale };
	const double elevation = std::atan2(anchor[2], std::sqrt(anchor[0] * anchor[0] + anchor[1] * anchor[1])); // the anchor's angle above the eye
	double tilt = std::clamp(_pitch, -CardTiltMax * Pi / 180.0, CardTiltUpMax * Pi / 180.0);
	if (elevation > CardTiltUpMax * Pi / 180.0)
	{
		tilt = std::max(tilt, std::min(elevation - CardTiltUpMax * Pi / 180.0, CardTiltSteepMax * Pi / 180.0));
	}
	const double cardUp[3] = { -std::sin(tilt) * level[0], -std::sin(tilt) * level[1], std::cos(tilt) };
	const double cardFacing[3] = { std::cos(tilt) * level[0], std::cos(tilt) * level[1], std::sin(tilt) };
	const double anchorAcross = dot(anchor, _levelRight), anchorFacing = dot(anchor, cardFacing);
	// A sprite pixel's size (camera units): across, up the card (squashed for something that
	// must fit under a floor), and along the floor for a lying row; and the floor lying rows lie
	// on, half a voxel above the anchor (eye height 0), rising towards the camera with the ground
	// in front of it (a step, a slope: see lyingRise), so much higher for each unit nearer.
	const double columnSize = scale * SpritePixelX, rowSize = scale * ZScale * draw.standingSquash, lyingRowSize = scale * LyingRowVoxels;
	const double floorHeight = anchor[2] + 0.5 * ZScale;
	const double floorSlope = draw.lyingSlope * ZScale;
	const double anchorAlong = anchor[0] * level[0] + anchor[1] * level[1]; // how far ahead the anchor is, along the ground
	const bool lying = pose == SpritePose::Lying;

	// Where a ray meets the sprite: the ray scaled to reach one unit ahead (so how far along it
	// is also the view depth), and whether it passes below the anchor. A lying sprite's lower
	// rows are met on the floor in front of the anchor, and never from under that floor; the
	// rest on the card. Gives the distance and the sprite row there; false when the ray misses
	// the sprite.
	auto meetSprite = [&](const double ray[3], bool belowAnchor, double &distance, double &spriteRow) {
		distance = -1.0;
		spriteRow = 0.0;
		if (lying && belowAnchor && floorHeight >= 0.0)
		{
			return false;
		}
		// The floor's height towards the camera: floorHeight + floorSlope * (anchorAlong - along),
		// along being how far ahead the ray's point is; where the ray comes down onto it.
		const double rayAlong = ray[0] * level[0] + ray[1] * level[1];
		const double descent = ray[2] + floorSlope * rayAlong;
		if (lying && belowAnchor && descent < 0.0 && floorHeight < 0.0)
		{
			distance = (floorHeight + floorSlope * anchorAlong) / descent;
			const double towardsCamera = anchorAlong - rayAlong * distance;
			spriteRow = anchorY + towardsCamera / lyingRowSize;
		}
		if (spriteRow < anchorY || distance < 0.0)
		{
			const double facing = dot(ray, cardFacing);
			if (facing <= 1e-9) return false;
			distance = anchorFacing / facing;
			const double fromAnchor[3] = { ray[0] * distance - anchor[0], ray[1] * distance - anchor[1], ray[2] * distance - anchor[2] };
			spriteRow = anchorY - dot(fromAnchor, cardUp) / rowSize;
		}
		return distance >= 1.0 && spriteRow >= 0.0 && spriteRow < spriteHeight;
	};
	// Puts one sprite pixel on the screen, shaded, where nothing nearer is drawn already, and
	// notes its depth so that farther sprites drawn later are hidden by it. A sprite pixel covers
	// whatever shone behind it, so that no longer glows (see drawLightHalos). A unit's pixel
	// notes that it may be veiled only thinly by see-through smoke, and, near where the player
	// points, whose picture it is (a body's too, on the floor); anything else solid drawn over it
	// takes these back.
	// A see-through pixel (opacity below SeeThroughLevels) is mixed over what's there instead,
	// within what's left of a unit's allowance, and leaves the depth alone; it only stops the
	// glow behind it when it is more than half opaque.
	auto plot = [&](int pixelX, int pixelY, Uint8 color, float depth, int pixelOpacity) {
		const int index = pixelY * width + pixelX;
		float &depthHere = _depthBuffer[index];
		if (!color || depth >= depthHere) return;
		if (pixelOpacity < SeeThroughLevels)
		{
			Uint8 &veilLeft = _unitVeil[index];
			pixelOpacity = std::min(pixelOpacity, (int)veilLeft);
			if (pixelOpacity <= 0) return;
			if (veilLeft != 255) veilLeft -= pixelOpacity;
			Uint8 *pixel = getRaw(pixelX, pixelY);
			*pixel = seeThrough(pixelOpacity, shadeColor(color, shade), *pixel);
			if (pixelOpacity > SeeThroughLevels / 2) _glowMask[index] = 0;
			return;
		}
		if (opacity < 1.0f && dither(pixelX, pixelY) >= opacity) return;
		*getRaw(pixelX, pixelY) = shadeColor(color, shade);
		depthHere = depth;
		_glowMask[index] = 0;
		_unitVeil[index] = draw.unit ? UnitVeilMax : 255;
		const int patchX = pixelX - _pointerX + PointerReach, patchY = pixelY - _pointerY + PointerReach;
		if (patchX >= 0 && patchY >= 0 && patchX <= 2 * PointerReach && patchY <= 2 * PointerReach)
		{
			_pointerUnits[patchY][patchX] = draw.unit;
			_pointerBodies[patchY][patchX] = draw.body;
		}
	};
	const SDL_Surface *spriteSurface = const_cast<Surface*>(sprite)->getSurface(); // read only
	auto spritePixel = [&](int spriteX, int spriteY) { return ((const Uint8 *)spriteSurface->pixels)[spriteY * spriteSurface->pitch + spriteX]; };
	// How opaque a sprite pixel is, in see-through steps (SeeThroughLevels for a solid sprite).
	auto pixelOpacityAt = [&](int spriteX, int spriteY) {
		return seeThroughSprite ? (int)(seeThroughSprite->opacityMap[spriteY * spriteWidth + spriteX] * seeThroughOpacity + 0.5f) : SeeThroughLevels;
	};

	// The view tilted by a lean (see setRoll): the screen's rows no longer run level across the
	// card, so each pixel is worked out on its own, the same way: its ray, where that meets the
	// sprite, and the sprite's column from how far across (level, square to the view) it does.
	if (_roll != 0.0)
	{
		// The pixels to look at: the box round the sprite's corners on the screen, the card's
		// and, for a lying sprite, the floor's in front of the anchor (all of the view if one is
		// behind the camera; the rays find what is really there).
		int left = width, right = 0, top = height, bottom = 0;
		auto addCorners = [&](const double middle[3]) {
			for (const double across : { -anchorX * columnSize, (spriteWidth - anchorX) * columnSize })
			{
				const double corner[3] = { middle[0] + _levelRight[0] * across, middle[1] + _levelRight[1] * across, middle[2] + _levelRight[2] * across };
				const double depth = dot(corner, _forward);
				if (depth <= 1.0)
				{
					left = top = 0; right = width; bottom = height;
					continue;
				}
				const double screenX = width / 2.0 + dot(corner, _right) * focalLength / depth, screenY = height / 2.0 - dot(corner, _up) * focalLength / depth;
				left = std::min(left, (int)std::floor(screenX)); right = std::max(right, (int)std::ceil(screenX));
				top = std::min(top, (int)std::floor(screenY)); bottom = std::max(bottom, (int)std::ceil(screenY));
			}
		};
		double topMiddle[3], bottomMiddle[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			topMiddle[axis] = anchor[axis] + cardUp[axis] * anchorY * rowSize;
			bottomMiddle[axis] = anchor[axis] - cardUp[axis] * (spriteHeight - anchorY) * rowSize;
		}
		addCorners(topMiddle);
		if (lying)
		{
			const double farFloor[3] = { anchor[0], anchor[1], floorHeight };
			const double nearFloor[3] = { anchor[0] - level[0] * (spriteHeight - anchorY) * lyingRowSize,
				anchor[1] - level[1] * (spriteHeight - anchorY) * lyingRowSize, floorHeight + floorSlope * (spriteHeight - anchorY) * lyingRowSize };
			addCorners(anchor);
			addCorners(farFloor);
			addCorners(nearFloor);
		}
		else
		{
			addCorners(bottomMiddle);
		}
		left = std::max(0, left); top = std::max(std::max(0, bandTop), top); right = std::min(width, right); bottom = std::min(std::min(height, bandBottom), bottom);
		// How far up the anchor is on the image plane without the tilt: rays below that pass
		// below the anchor.
		const double anchorRise = dot(anchor, _levelUp) / dot(anchor, _forward);
		for (int pixelY = top; pixelY < bottom; ++pixelY)
		{
			const double rise = (height / 2.0 - (pixelY + 0.5)) / focalLength;
			for (int pixelX = left; pixelX < right; ++pixelX)
			{
				const double across = (pixelX + 0.5 - width / 2.0) / focalLength;
				const double ray[3] = { _forward[0] + across * _right[0] + rise * _up[0], _forward[1] + across * _right[1] + rise * _up[1],
					_forward[2] + across * _right[2] + rise * _up[2] };
				double distance, spriteRow;
				if (!meetSprite(ray, dot(ray, _levelUp) < anchorRise, distance, spriteRow)) continue;
				const int spriteX = (int)std::floor(anchorX + (dot(ray, _levelRight) * distance - anchorAcross) / columnSize);
				if (spriteX < 0 || spriteX >= spriteWidth) continue;
				const int spriteY = (int)std::floor(spriteRow);
				plot(pixelX, pixelY, spritePixel(spriteX, spriteY), (float)std::max(1.0, distance - depthBias), pixelOpacityAt(spriteX, spriteY));
			}
		}
		return;
	}

	// The screen rows it may cover: from its top row's down to its bottom row's (an end behind
	// the camera reaches to the view's edge).
	auto screenRowOf = [&](const double point[3], double behindCamera) {
		const double depth = dot(point, _forward);
		return depth > 1.0 ? height / 2.0 - dot(point, _up) * focalLength / depth : behindCamera;
	};
	double topPoint[3], bottomPoint[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		topPoint[axis] = anchor[axis] + cardUp[axis] * anchorY * rowSize;
		bottomPoint[axis] = lying ? (axis < 2 ? anchor[axis] - level[axis] * (spriteHeight - anchorY) * lyingRowSize : floorHeight)
			: anchor[axis] - cardUp[axis] * (spriteHeight - anchorY) * rowSize;
	}
	// (Seen from below a lying sprite's floor, the floor in front of the anchor shows above the
	// anchor: the card's rows still reach down to the anchor.)
	double bottomRow = screenRowOf(bottomPoint, height);
	if (lying)
	{
		bottomRow = std::max(bottomRow, anchorScreenY + 1.0);
	}
	const int top = std::max(std::max(0, bandTop), (int)std::floor(screenRowOf(topPoint, 0.0)));
	const int bottom = std::min(std::min(height, bandBottom), (int)std::ceil(bottomRow));

	// The coloured part of a sprite row, from its first coloured pixel up to (not including) the
	// one after its last: most of a unit's frame is transparent round the unit, and up close
	// each of its pixels covers many on the screen. Found the first time a row is needed;
	// the screen rows showing the same sprite row share it.
	int spanRow = -1, spanFirst = 0, spanEnd = 0;
	auto findSpan = [&](int spriteY) {
		const Uint8 *row = (const Uint8 *)spriteSurface->pixels + spriteY * spriteSurface->pitch;
		spanFirst = 0;
		spanEnd = spriteWidth;
		while (spanFirst < spanEnd && !row[spanFirst]) ++spanFirst;
		while (spanEnd > spanFirst && !row[spanEnd - 1]) --spanEnd;
		spanRow = spriteY;
	};

	// Row by row: the ray through the middle of the row, where it meets the sprite, and along
	// the row, at that distance, where the anchor's column is and how wide a sprite pixel is
	// (the card and the floor both run square across the view).
	for (int pixelY = top; pixelY < bottom; ++pixelY)
	{
		const double rise = (height / 2.0 - (pixelY + 0.5)) / focalLength;
		const double ray[3] = { _forward[0] + rise * _up[0], _forward[1] + rise * _up[1], _forward[2] + rise * _up[2] };
		double distance, spriteRow;
		if (!meetSprite(ray, pixelY + 0.5 > anchorScreenY, distance, spriteRow)) continue;
		const int spriteY = (int)std::floor(spriteRow);
		if (spriteY != spanRow) findSpan(spriteY);
		if (spanFirst >= spanEnd) continue; // nothing coloured in this row
		// The screen pixels the sprite's row covers, cut down to its span's (a pixel more on each
		// side: each pixel's own column is worked out below, and anything outside the span is
		// transparent anyway, so the edges never depend on rounding).
		const double pixelsPerColumn = focalLength / distance * columnSize;
		const double rowAnchorX = width / 2.0 + anchorAcross * focalLength / distance;
		const int left = std::max(std::max(0, (int)std::floor(rowAnchorX - anchorX * pixelsPerColumn)),
			(int)std::floor(rowAnchorX + (spanFirst - anchorX) * pixelsPerColumn) - 1);
		const int right = std::min(std::min(width, (int)std::ceil(rowAnchorX + (spriteWidth - anchorX) * pixelsPerColumn)),
			(int)std::ceil(rowAnchorX + (spanEnd - anchorX) * pixelsPerColumn) + 1);
		const float rowDepth = (float)std::max(1.0, distance - depthBias);
		for (int pixelX = left; pixelX < right; ++pixelX)
		{
			const int spriteX = (int)std::floor(anchorX + (pixelX + 0.5 - rowAnchorX) / pixelsPerColumn);
			if (spriteX < 0 || spriteX >= spriteWidth) continue;
			plot(pixelX, pixelY, spritePixel(spriteX, spriteY), rowDepth, pixelOpacityAt(spriteX, spriteY));
		}
	}
}

/**
 * Draws a sprite standing in the world (see drawSprite above), given piece by piece: something
 * that isn't a unit, all of it.
 */
void FirstPersonView::drawSprite(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade,
	double scale, SpritePose pose, const SeeThroughSprite *seeThroughSprite)
{
	SpriteDraw draw;
	draw.sprite = sprite;
	draw.anchorX = anchorX;
	draw.anchorY = anchorY;
	std::copy(world, world + 3, draw.world);
	draw.depthBias = depthBias;
	draw.shade = shade;
	draw.scale = scale;
	draw.pose = pose;
	draw.seeThrough = seeThroughSprite;
	drawSprite(draw);
}

/**
 * Draws the recorded steps (see DrawStep) on all the view's threads, each a band of rows at a
 * time (see drawListBands), and empties the list.
 */
void FirstPersonView::replayDrawList()
{
	if (!_drawList.empty())
	{
		runOnAllThreads(&FirstPersonView::drawListBands);
	}
	_drawList.clear();
}

/**
 * The recorded steps over bands of the view's rows, taken one at a time from the shared counter
 * until none are left: a job for all the view's threads (see runOnAllThreads). Each band gets
 * every step in the order recorded, so every pixel comes out just as if one thread drew them
 * all, one after another.
 */
void FirstPersonView::drawListBands()
{
	const int bandCount = (getHeight() + BandRows - 1) / BandRows;
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		const int bandTop = band * BandRows, bandBottom = bandTop + BandRows;
		for (const DrawStep &step : _drawList)
		{
			switch (step.kind)
			{
			case DrawStep::Sprite:
				drawSprite(step.sprite, bandTop, bandBottom);
				break;
			case DrawStep::GroundShade:
				drawGroundShade(step.centre, step.radius, step.steps, step.density, bandTop, bandBottom);
				break;
			case DrawStep::Line:
				drawLine3D(step.from, step.to, step.color, step.thickness, step.opacity, 0.0f, bandTop, bandBottom, step.glows);
				break;
			}
		}
	}
}

/**
 * Where something standing on the ground at a point stands in the view: the point raised out
 * of the solid voxels it is in, by at most StandRaiseMax voxels. The game stands units and items
 * at the foot of a tile's floor, where the isometric art draws the floor's surface; the 3D view
 * builds the floor from its line-of-fire shape, a slab two voxels thick, so drawn at the foot
 * they would sink into it. (A point deeper in something solid, such as a wall, stays put.)
 * @param world The point on the ground, in voxels.
 * @return The height to draw it standing at, in voxels.
 */
double FirstPersonView::standingHeight(const double world[3]) const
{
	const int raise = riseOutOfSolid(world, StandRaiseMax);
	return raise > 0 ? (int)std::floor(world[2]) + raise : world[2];
}

/**
 * How far a point is raised out of the solid voxels it's in: the first voxel above it (or its
 * own) that nothing of the view's terrain fills, at most a number of voxels up.
 * @param world The point, in voxels.
 * @param maxRaise The most voxels to look up.
 * @return The voxels up to the open (0: it's in the open, or off the map); -1 if it's solid all
 *         that way (deep in a wall, say).
 */
int FirstPersonView::riseOutOfSolid(const double world[3], int maxRaise) const
{
	const int voxelX = (int)std::floor(world[0]), voxelY = (int)std::floor(world[1]), voxelZ = (int)std::floor(world[2]);
	if (voxelX < 0 || voxelY < 0 || voxelZ < 0 || voxelX >= _save->getMapSizeX() * 16 || voxelY >= _save->getMapSizeY() * 16)
	{
		return 0;
	}
	for (int raise = 0; raise <= maxRaise; ++raise)
	{
		const int z = voxelZ + raise;
		if (z >= _save->getMapSizeZ() * 24) break;
		if (!solidVoxel(voxelX, voxelY, z))
		{
			return raise;
		}
	}
	return -1;
}

/**
 * Whether a voxel is filled by the view's terrain: any of its tile's parts (floor, walls,
 * object) as the view builds them.
 * @param voxelX, voxelY, voxelZ The voxel.
 * @return True if it's solid; false if it's open, or off the map.
 */
bool FirstPersonView::solidVoxel(int voxelX, int voxelY, int voxelZ) const
{
	if (voxelX < 0 || voxelY < 0 || voxelZ < 0 || voxelX >= _save->getMapSizeX() * 16 || voxelY >= _save->getMapSizeY() * 16 || voxelZ >= _save->getMapSizeZ() * 24)
	{
		return false;
	}
	const TileView &view = _tileViews[_save->getTileIndex(Position(voxelX / 16, voxelY / 16, voxelZ / 24))];
	for (const VoxelModel *part : view.part)
	{
		if (part && part->color[voxelZ % 24][voxelY % 16][voxelX % 16])
		{
			return true;
		}
	}
	return false;
}

/**
 * The ground under something lying at a point (a player: items on stairs and slopes, seen from
 * the side, sank half into them): the plane that best fits the ground's surface round it (least
 * squares), from 5x5 points out to a reach each way: at each, the top of the first solid voxel
 * going down from GroundPlaneProbe voxels above the point's floor to as far below it (a point
 * that starts inside something solid, a wall or a tall object beside it, or finds nothing in
 * that range, is left out; below the map's bottom the bottom counts), lifted onto the highest of
 * those points above it (as something flat rests on the edges of steps). On a flat floor that's
 * the floor's top and no slope; on a ramp its slant; on stairs, the steps' slant, on their edges.
 * @param world The point (an item's anchor on its floor), in voxels.
 * @param reach How far out from it the points go each way, in voxels.
 * @param height Set to the plane's height at the point, in voxels.
 * @param slope Set to the plane's slope: voxels up for each voxel along x and along y, each at
 *        most LyingSlopeMax.
 * @return False if too few points found the ground for a plane (nothing set).
 */
bool FirstPersonView::groundPlane(const double world[3], double reach, double &height, double slope[2]) const
{
	// The sums for the least squares fit of surface = height + slope x * dx + slope y * dy, and
	// the points themselves (for the lift onto the highest, below).
	double sumX = 0.0, sumY = 0.0, sumZ = 0.0, sumXX = 0.0, sumYY = 0.0, sumXY = 0.0, sumXZ = 0.0, sumYZ = 0.0;
	int count = 0;
	struct GroundPoint { double dx, dy, surface; };
	GroundPoint points[25];
	const int probeTop = (int)std::floor(world[2]) + GroundPlaneProbe;
	for (int stepY = -2; stepY <= 2; ++stepY)
	{
		for (int stepX = -2; stepX <= 2; ++stepX)
		{
			const double dx = stepX * reach / 2.0, dy = stepY * reach / 2.0;
			const int voxelX = (int)std::floor(world[0] + dx), voxelY = (int)std::floor(world[1] + dy);
			if (voxelX < 0 || voxelY < 0 || voxelX >= _save->getMapSizeX() * 16 || voxelY >= _save->getMapSizeY() * 16 || solidVoxel(voxelX, voxelY, probeTop))
			{
				continue;
			}
			int surface = -1;
			for (int z = probeTop - 1; z >= probeTop - 2 * GroundPlaneProbe && surface < 0; --z)
			{
				if (z < 0 || solidVoxel(voxelX, voxelY, z))
				{
					surface = std::max(z + 1, 0);
				}
			}
			if (surface < 0)
			{
				continue;
			}
			sumX += dx; sumY += dy; sumZ += surface;
			sumXX += dx * dx; sumYY += dy * dy; sumXY += dx * dy; sumXZ += dx * surface; sumYZ += dy * surface;
			points[count++] = { dx, dy, (double)surface };
		}
	}
	if (count < 6)
	{
		return false;
	}
	// The normal equations, solved by Cramer's rule.
	const double matrix[3][3] = { { (double)count, sumX, sumY }, { sumX, sumXX, sumXY }, { sumY, sumXY, sumYY } };
	const double right[3] = { sumZ, sumXZ, sumYZ };
	auto determinant = [](const double m[3][3]) {
		return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	};
	const double whole = determinant(matrix);
	if (std::abs(whole) < 1e-9)
	{
		return false; // (the points all in a line)
	}
	double solution[3];
	for (int column = 0; column < 3; ++column)
	{
		double replaced[3][3];
		for (int row = 0; row < 3; ++row)
		{
			for (int col = 0; col < 3; ++col)
			{
				replaced[row][col] = col == column ? right[row] : matrix[row][col];
			}
		}
		solution[column] = determinant(replaced) / whole;
	}
	slope[0] = std::clamp(solution[1], -LyingSlopeMax, LyingSlopeMax);
	slope[1] = std::clamp(solution[2], -LyingSlopeMax, LyingSlopeMax);
	// Lifted onto the highest points above it (at most GroundPlaneLiftMax voxels): something flat
	// on steps rests on their edges, and the voxel steps of a ramp stand out of the average.
	double lift = 0.0;
	for (int point = 0; point < count; ++point)
	{
		lift = std::max(lift, points[point].surface - (solution[0] + slope[0] * points[point].dx + slope[1] * points[point].dy));
	}
	height = solution[0] + std::min(lift, (double)GroundPlaneLiftMax);
	return true;
}

/**
 * How steeply the ground in front of something lying at a point rises towards the camera (a step
 * of stairs, a slope; a player's report: items there sank into them): looked at every 2 voxels
 * along the floor towards the camera, within the lying rows' reach, how far the solid there rises
 * above the item's floor (up to LyingRiseMax), and the steepest rise for the way (at most
 * LyingSlopeMax). Ground that only falls, or is level, gives none.
 * @param world Where it lies (the anchor, on its floor), in voxels.
 * @param reach How far towards the camera its lying rows reach, in voxels.
 * @return Voxels up for each voxel towards the camera.
 */
double FirstPersonView::lyingRise(const double world[3], double reach) const
{
	double steepest = 0.0;
	const double towardsCamera[2] = { -std::cos(_yaw), -std::sin(_yaw) };
	for (double along = 2.0; along <= reach + 0.01; along += 2.0)
	{
		const double point[3] = { world[0] + towardsCamera[0] * along, world[1] + towardsCamera[1] * along, world[2] + 0.5 };
		const int rise = riseOutOfSolid(point, LyingRiseMax);
		if (rise > 0)
		{
			steepest = std::max(steepest, rise / along);
		}
	}
	return std::min(steepest, LyingSlopeMax);
}

/**
 * Watches the units for hits and kills, once a frame, by their health and whether they are
 * out (dead or unconscious) compared with the last frame. A hit unit flinches back from the
 * camera and flashes (see drawUnits); one in sight sprays blood (or sparks, from machines);
 * and an enemy hit or downed in sight marks the crosshair (the hitmarker).
 */
void FirstPersonView::watchUnits()
{
	typedef FirstPersonParticles Particles;
	const Uint32 now = SDL_GetTicks();
	for (BattleUnit *unit : *_save->getUnits())
	{
		// A unit not watched yet: just remember how it is.
		const bool isOut = unit->isOut();
		auto found = _watchedUnits.find(unit);
		if (found == _watchedUnits.end())
		{
			_watchedUnits.emplace(unit, UnitWatch{ unit->getHealth(), isOut, 0, { 0.0f, 0.0f } });
			continue;
		}
		// Did it lose health, or go down (killed or knocked out), since the last frame?
		UnitWatch &watch = found->second;
		const bool wasHit = unit->getHealth() < watch.health;
		const bool killed = isOut && !watch.out;
		watch.health = unit->getHealth();
		watch.out = isOut;
		if (!wasHit && !killed)
		{
			continue;
		}
		// Its chest (the middle of its tiles, 60% of its height up), and the level direction
		// from the camera to it: dx, dy from the eye, flatDistance their length.
		const Position tilePosition = unit->getPosition();
		const int unitSize = unit->getArmor()->getSize(); // tiles across: 1, or 2 for big units
		const float chest[3] = { tilePosition.x * 16.0f + 8.0f * unitSize, tilePosition.y * 16.0f + 8.0f * unitSize,
			tilePosition.z * 24.0f + unit->getFloatHeight() + unit->getHeight() * 0.6f };
		const float dx = chest[0] - (float)_eye[0], dy = chest[1] - (float)_eye[1];
		const float flatDistance = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
		// A hit starts the flinch, pushing it away from the camera.
		if (wasHit)
		{
			watch.hitTick = now;
			watch.push[0] = dx / flatDistance;
			watch.push[1] = dy / flatDistance;
		}
		const bool seen = unit->getVisible() || _save->getDebugMode();
		if (seen && unit != _cameraUnit)
		{
			// Blood or sparks, thrown away from the camera: 14 particles of two colours,
			// blood lit by the world, sparks shining by themselves, falling under gravity.
			const bool flesh = unit->isWoundable();
			const Uint8 sprayColors[2] = { flesh ? _colors.flareFire[0] : _colors.spark,
				flesh ? shadeColor(_colors.flareFire[0], 5) : _colors.fire[1] };
			for (int i = 0; i < 14; ++i)
			{
				Particles::Particle particle;
				std::copy(chest, chest + 3, particle.pos);
				const float speed = _particles.random(20, 60);
				particle.vel[0] = dx / flatDistance * speed + _particles.random(-15, 15);
				particle.vel[1] = dy / flatDistance * speed + _particles.random(-15, 15);
				particle.vel[2] = _particles.random(-5, 30);
				particle.age = 0; particle.life = _particles.random(0.3f, 0.8f); particle.gravity = 160; particle.drag = 0.8f;
				particle.color = sprayColors[i & 1]; particle.size = 2;
				particle.kind = flesh ? Particles::LIT : Particles::GLOW; particle.group = Particles::EFFECT;
				_particles.add(particle);
			}
		}
		// An enemy hit or downed in sight: the hitmarker on the crosshair.
		if (seen && unit->getFaction() == FACTION_HOSTILE)
		{
			_hitMarkTick = now;
			_hitMarkKill = killed;
		}
	}
}

/**
 * Draws the visible units as billboards: the game's own unit sprites, standing where the unit
 * is, turned to show the side facing the camera. Walking units glide between tiles, and every
 * unit eases between the game's small steps; a hit unit flinches and flashes; each stands on a
 * soft shadow. Also notes where the numbered enemy markers go this frame (drawMarkers).
 * The unit being looked through is not drawn. Their pixels are noted as a unit's, so the
 * see-through smoke drawn later veils them only thinly (see _unitVeil).
 * Everything is worked out here, on the game's thread, and recorded (see DrawStep); then all of
 * it is drawn at once on all the view's threads (see replayDrawList).
 */
void FirstPersonView::drawUnits()
{
	// The sprite drawer, with the diving helmets on under the sea, on the game's own animation
	// counter (a frame every 100 ms), as the map draws: the 8-frame cycles, and the mods'
	// recolour scripts, which may count much longer ones (a wounded unit blinking red and white).
	UnitSprite unitSprite(_spriteBuffer, _game->getMod(), _save, _save->getAnimFrame(), _save->getDepth() != 0, 0, 0);
	const bool debug = _save->getDebugMode();
	_markerSpots.clear();
	_facingSpots.clear();
	// Time since the last frame in seconds (at most a tenth, none the first time), for the gliding.
	const Uint32 now = SDL_GetTicks();
	const double elapsedSeconds = _unitLastTick ? std::min<Uint32>(100, now - _unitLastTick) / 1000.0 : 0.0;
	_unitLastTick = now;
	// The units' size (setting "Unit size", 80 to 100% of the isometric map's: smaller, standing
	// on the same spot), and their hitboxes (setting): always, or fading in and out once a second.
	const double unitScale = std::clamp(Options::fpsUnitSize, 80, 100) / 100.0;
	const float hitboxOpacity = Options::fpsHitboxes == 2 ? 1.0f
		: Options::fpsHitboxes == 1 ? 0.5f + 0.5f * (float)std::cos((now % 1000) / 1000.0 * 2.0 * Pi) : 0.0f;

	// The units are recorded first (their shadows, sprites and hitboxes, in order), then drawn
	// on all the view's threads at once (see replayDrawList). Each unit's frame is composed in
	// the sprite buffer, then kept in a frame of its own until then.
	int framesUsed = 0;
	for (BattleUnit *unit : *_save->getUnits())
	{
		// Only units still in the fight (not dead or unconscious), not the one looked through,
		// and in sight (any unit in debug mode).
		if (unit->isOut() || unit == _cameraUnit || (!debug && !unit->getVisible()))
		{
			continue;
		}
		const int unitSize = unit->getArmor()->getSize(); // tiles across: 1, or 2 for big units
		// Where the unit's feet are, in voxels: the middle of its tiles, on the floor (raised
		// by the tile's terrain level, such as rubble or a slope).
		double feet[3];
		auto place = [&](Position tilePosition, double out[3]) {
			const Tile *tile = _save->getTile(tilePosition);
			out[0] = tilePosition.x * 16.0 + 8.0 * unitSize;
			out[1] = tilePosition.y * 16.0 + 8.0 * unitSize;
			out[2] = tilePosition.z * 24.0 - (tile ? tile->getTerrainLevel() : 0);
		};
		if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
		{
			// On the move: between the tile it left and the one it's going to, by how far into
			// the step the game is (a diagonal step takes twice as many phases, 16 instead of 8).
			double fromFeet[3], toFeet[3];
			place(unit->getLastPosition(), fromFeet);
			place(unit->getDestination(), toFeet);
			const int direction = unit->getDirection();
			const double walkPhase = unit->getWalkingPhase() + unit->getDiagonalWalkingPhase();
			const double progress = std::clamp(walkPhase / ((direction % 2) ? 16.0 : 8.0), 0.0, 1.0);
			for (int axis = 0; axis < 3; ++axis) feet[axis] = fromFeet[axis] + (toFeet[axis] - fromFeet[axis]) * progress;
		}
		else
		{
			place(unit->getPosition(), feet);
		}
		// The game moves units in small steps a few times a second: glide between them
		// (but snap on anything big: a unit seen anew, teleported, falling a floor).
		Glide &glide = _unitGlides[unit];
		const double jump = std::abs(glide.pos[0] - feet[0]) + std::abs(glide.pos[1] - feet[1]) + std::abs(glide.pos[2] - feet[2]);
		if (now - glide.seen > 250 || jump > 24.0)
		{
			std::copy(feet, feet + 3, glide.pos);
		}
		else
		{
			// Close part of the gap each frame, more after a longer frame (all of it once a
			// frame takes 1/18 of a second).
			const double follow = std::min(1.0, elapsedSeconds * 18.0);
			for (int axis = 0; axis < 3; ++axis)
			{
				glide.pos[axis] += (feet[axis] - glide.pos[axis]) * follow;
				feet[axis] = glide.pos[axis];
			}
		}
		glide.seen = now;
		// A hit unit flinches back from the camera for a moment (220 ms, up to 3 voxels), and
		// flashes (with muzzle flashes on).
		float flash = 0.0f;
		auto watched = _watchedUnits.find(unit);
		if (watched != _watchedUnits.end() && watched->second.hitTick && now - watched->second.hitTick < 220)
		{
			const float flinch = 1.0f - (now - watched->second.hitTick) / 220.0f; // 1 at the hit, down to 0
			feet[0] += watched->second.push[0] * 3.0 * flinch;
			feet[1] += watched->second.push[1] * 3.0 * flinch;
			flash = Options::fpsMuzzleFlash ? flinch : 0.0f;
		}

		// A marked enemy's marker goes 5 voxels above its head.
		for (const auto &marker : _markers)
		{
			if (marker.first == unit)
			{
				MarkerSpot spot = { { feet[0], feet[1], feet[2] + unit->getFloatHeight() + unit->getHeight() + 5.0 }, marker.second };
				_markerSpots.push_back(spot);
			}
		}

		// The isometric sprites show units as seen from the south-east (direction 3).
		// Rotate the unit's facing so the side turned towards the camera is shown: the
		// direction from the unit to the camera, in the game's eight directions (0 north,
		// clockwise; atan2 measures from east, so a quarter turn is added).
		const double toCameraDegrees = std::atan2(_eye[1] - feet[1], _eye[0] - feet[0]) * 180.0 / Pi;
		const int cameraDirection = (((int)std::lround((toCameraDegrees + 90.0) / 45.0)) % 8 + 8) % 8;
		unitSprite.setViewRotation(3 - cameraDirection);

		// The light on it, at its chest: the game's light, lamps, flashlight and flares, and
		// the flash of a hit.
		_spriteBuffer->clear();
		const float chest[3] = { (float)feet[0], (float)feet[1], (float)feet[2] + 12.0f };
		const int shade = (int)std::lround(std::clamp(lightAt(chest[0], chest[1], chest[2]) - pointLight(chest, nullptr, false)
			- flareLight(chest, nullptr, false) * 8.0f - flash * 6.0f, 0.0f, 15.0f));
		if (Options::fpsShadows)
		{
			recordUnitShadow(feet, 5.5 * unitSize); // flying units too: the shadow on the ground shows where they hover
		}
		// Compose the unit's frame in the sprite buffer and keep it in a frame of its own, then
		// record it standing in the world, on the floor's surface (the hitbox and the markers
		// keep the game's own heights). (Composed unlit and shaded when placed: an upscaled frame
		// then serves in any light.)
		const GraphSubset mask(SpriteBufferSize, SpriteBufferSize);
		if (unitSize == 1)
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
		}
		else
		{
			// The parts are laid out on screen like the 2x2 tiles they cover, back to front.
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
			unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
			unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
		}
		DrawStep step;
		step.kind = DrawStep::Sprite;
		step.sprite.sprite = keepComposedFrame(framesUsed);
		step.sprite.anchorX = 32;
		step.sprite.anchorY = unitSize == 1 ? 40 : 48; // the centre of the floor diamond; a big unit's, the corner its four tiles share
		step.sprite.world[0] = feet[0];
		step.sprite.world[1] = feet[1];
		step.sprite.world[2] = standingHeight(feet);
		step.sprite.shade = shade;
		step.sprite.scale = unitScale;
		step.sprite.unit = unit;
		useSmoothVersion(step.sprite);
		_drawList.push_back(step);
		if (hitboxOpacity > 0.05f)
		{
			recordHitbox(unit, feet, hitboxOpacity);
		}
		// Which way it faces, with the key for it held (see drawFacing): round its footprint (inside
		// its tiles, so neighbours' rings don't cross), at the bottom of its hitbox (under a flyer),
		// in the colour of its side.
		if (_showFacing)
		{
			FacingSpot spot = { { feet[0], feet[1], feet[2] + unit->getFloatHeight() + 1.0 }, 8.0 * unitSize - 1.5, unit->getDirection(),
				_colors.hitbox[unit->getFaction() == FACTION_PLAYER ? 0 : unit->getFaction() == FACTION_HOSTILE ? 1 : 2] };
			_facingSpots.push_back(spot);
		}
	}
	// Then draw them all.
	replayDrawList();
}

/**
 * Outlines a unit's hitbox (setting "Hitboxes"), recorded as lines to be drawn with the units
 * (see drawUnits): the voxels the game counts as the unit when a shot or a blow reaches it,
 * which the sprite only hints at. As the game has it (see
 * TileEngine::voxelCheck), the unit is its armor's footprint (a LOFT template for each tile it
 * covers, read mirrored on x, the four parts of a big unit in the game's order) standing from
 * just above its floor (and its hovering height) up to its height, kneeling or standing. The
 * outline is the footprint's edge at the bottom and the top, with four upright edges at its
 * furthest points, in the colour of the unit's side; it is hidden behind walls and behind the
 * unit's own sprite like anything else in the world.
 * @param unit The unit.
 * @param feet Where it's drawn: the middle of its footprint on its floor, in voxels.
 * @param opacity How much of the outline shows (fading in and out with the blinking setting).
 */
void FirstPersonView::recordHitbox(const BattleUnit *unit, const double feet[3], float opacity)
{
	const std::vector<Uint16> *loftData = _game->getMod()->getVoxelData();
	const int size = unit->getArmor()->getSize(), cells = 16 * size;
	// The footprint, one voxel column per cell: solid where the part's LOFT row has the bit.
	auto solid = [&](int x, int y) -> bool {
		if (x < 0 || y < 0 || x >= cells || y >= cells) return false;
		static const int bigParts[] = { 1, 0, 3, 2 }; // the game's order for a 2x2 unit's parts
		const int part = size > 1 ? bigParts[x / 16 + (y / 16) * 2] : 0;
		const size_t row = (size_t)unit->getLoftemps(part) * 16 + y % 16;
		return row < loftData->size() && ((*loftData)[row] & (1 << (15 - x % 16)));
	};
	// Where it stands: the footprint's north-west corner, and the bottom and top of the column
	// (the game's solid voxels run from one above its floor and hovering height to its height).
	const float originX = (float)(feet[0] - 8.0 * size), originY = (float)(feet[1] - 8.0 * size);
	const float bottom = (float)(feet[2] + unit->getFloatHeight() + 1.0), top = bottom + unit->getHeight();
	const Uint8 color = _colors.hitbox[unit->getFaction() == FACTION_PLAYER ? 0 : unit->getFaction() == FACTION_HOSTILE ? 1 : 2];
	// Each edge is recorded as a line, to be drawn with the units (see drawUnits, replayDrawList).
	auto recordLine = [&](const float from[3], const float to[3]) {
		DrawStep step;
		step.kind = DrawStep::Line;
		std::copy(from, from + 3, step.from);
		std::copy(to, to + 3, step.to);
		step.color = color;
		step.thickness = 1;
		step.opacity = opacity;
		step.glows = Options::fpsGlowingOutlines; // (setting "Glowing outlines")
		_drawList.push_back(step);
	};

	// The footprint's edge, at the bottom and the top: each side of a solid cell with no solid
	// neighbour beyond it. Meanwhile, its furthest points on each axis, for the upright edges.
	float extremes[4][2] = { { 1e9f, 0 }, { -1e9f, 0 }, { 0, 1e9f }, { 0, -1e9f } }; // west, east, north, south
	for (int y = 0; y < cells; ++y)
	{
		for (int x = 0; x < cells; ++x)
		{
			if (!solid(x, y)) continue;
			const float x0 = originX + x, y0 = originY + y, x1 = x0 + 1.0f, y1 = y0 + 1.0f;
			const float sides[4][4] = { { x0, y0, x0, y1 }, { x1, y0, x1, y1 }, { x0, y0, x1, y0 }, { x0, y1, x1, y1 } };
			const bool open[4] = { !solid(x - 1, y), !solid(x + 1, y), !solid(x, y - 1), !solid(x, y + 1) };
			for (int side = 0; side < 4; ++side)
			{
				if (!open[side]) continue;
				for (float z : { bottom, top })
				{
					const float from[3] = { sides[side][0], sides[side][1], z }, to[3] = { sides[side][2], sides[side][3], z };
					recordLine(from, to);
				}
			}
			const float middleX = x0 + 0.5f, middleY = y0 + 0.5f;
			if (x0 < extremes[0][0]) { extremes[0][0] = x0; extremes[0][1] = middleY; }
			if (x1 > extremes[1][0]) { extremes[1][0] = x1; extremes[1][1] = middleY; }
			if (y0 < extremes[2][1]) { extremes[2][1] = y0; extremes[2][0] = middleX; }
			if (y1 > extremes[3][1]) { extremes[3][1] = y1; extremes[3][0] = middleX; }
		}
	}
	// The upright edges, at the furthest points west, east, north and south.
	for (const auto &point : extremes)
	{
		if (point[0] > 1e8f || point[0] < -1e8f || point[1] > 1e8f || point[1] < -1e8f) continue;
		const float from[3] = { point[0], point[1], bottom }, to[3] = { point[0], point[1], top };
		recordLine(from, to);
	}
}

/**
 * Finds the unit the player points at, once the frame's sprites are drawn (see
 * getUnitAtPointer): the one whose picture shows right where the player points, or else the
 * nearest one within PointerReach pixels of it (a unit far off is only a few pixels across).
 * Only what's drawn counts, so only units in sight, standing, and not hidden behind anything.
 * The same for a body lying on the floor (an unconscious unit's, see getBodyAtPointer), when
 * its picture is nearer the pointer than any standing unit's (a medi-kit's patient).
 */
void FirstPersonView::findUnitAtPointer()
{
	_unitAtPointer = nullptr;
	_bodyAtPointer = nullptr;
	int nearestUnit = 1 << 30, nearestBody = 1 << 30;
	for (int patchY = 0; patchY <= 2 * PointerReach; ++patchY)
	{
		for (int patchX = 0; patchX <= 2 * PointerReach; ++patchX)
		{
			const int distanceSq = (patchX - PointerReach) * (patchX - PointerReach) + (patchY - PointerReach) * (patchY - PointerReach);
			if (_pointerUnits[patchY][patchX] && distanceSq < nearestUnit)
			{
				nearestUnit = distanceSq;
				_unitAtPointer = _pointerUnits[patchY][patchX];
			}
			if (_pointerBodies[patchY][patchX] && distanceSq < nearestBody)
			{
				nearestBody = distanceSq;
				_bodyAtPointer = _pointerBodies[patchY][patchX];
			}
		}
	}
	if (nearestUnit <= nearestBody)
	{
		_bodyAtPointer = nullptr; // (a standing unit's picture is nearer, or as near)
	}
}

/**
 * A soft dark patch on the ground under a unit, so it stands on the floor instead of floating:
 * recorded, to be drawn with the units (see drawUnits, replayDrawList).
 * @param feet The point on the ground under the unit's middle, in voxels.
 * @param radius The patch's radius, in voxels.
 */
void FirstPersonView::recordUnitShadow(const double feet[3], double radius)
{
	DrawStep step;
	step.kind = DrawStep::GroundShade;
	std::copy(feet, feet + 3, step.centre);
	step.radius = radius;
	step.steps = 2;
	step.density = 1.8f;
	_drawList.push_back(step);
}

/**
 * Darkens the ground in a soft disc (unit shadows, scorched earth): pixels whose floor lies
 * within the disc lose up to `steps` shades, thinning out (dithered) towards the edge. Only
 * pixels showing the floor right there are darkened, not a wall or a unit in front of it.
 * @param centre The disc's centre on the ground, in voxels.
 * @param radius The disc's radius, in voxels.
 * @param steps How many shades darker it makes the ground.
 * @param density How solid it is: above 1 the middle is solid and only the rim thins out.
 * @param bandTop, bandBottom Only the screen rows from bandTop up to (not including) bandBottom
 *        are darkened (a band of the view, see replayDrawList).
 */
void FirstPersonView::drawGroundShade(const double centre[3], double radius, int steps, float density, int bandTop, int bandBottom)
{
	// The screen rectangle around the disc: the four corners of the square around it projected
	// (nothing drawn if any is behind the camera), cut to the view.
	const int width = getWidth(), height = getHeight();
	int left = width, top = height, right = -1, bottom = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double cornerPoint[3] = { centre[0] + ((corner & 1) ? radius : -radius), centre[1] + ((corner & 2) ? radius : -radius),
			centre[2] + 0.5 };
		double screenX, screenY, cornerDepth;
		if (!projectToScreen(cornerPoint, screenX, screenY, cornerDepth))
		{
			return;
		}
		left = std::min(left, (int)screenX); right = std::max(right, (int)screenX + 1);
		top = std::min(top, (int)screenY); bottom = std::max(bottom, (int)screenY + 1);
	}
	left = std::max(0, left); top = std::max(std::max(0, bandTop), top); right = std::min(width - 1, right); bottom = std::min(std::min(height, bandBottom) - 1, bottom);
	// Each pixel's ray meets the ground plane half a voxel above the centre: if that is within
	// the disc and it is the floor the pixel shows, darken the pixel.
	const double groundZ = centre[2] + 0.5;
	for (int pixelY = top; pixelY <= bottom; ++pixelY)
	{
		Uint8 *pixelRow = getRaw(0, pixelY);
		for (int pixelX = left; pixelX <= right; ++pixelX)
		{
			// The ray through the pixel, which must go down to meet the ground.
			double rayDirection[3];
			rayDirectionThrough(pixelX, pixelY, rayDirection);
			if (rayDirection[2] > -1e-4) continue;
			const double distanceAlongRay = (groundZ - _eye[2]) / rayDirection[2];
			if (distanceAlongRay <= 0.0) continue;
			// Where it meets the ground, from the disc's centre (offsetX, offsetY in voxels).
			const double offsetX = _eye[0] + rayDirection[0] * distanceAlongRay - centre[0],
				offsetY = _eye[1] + rayDirection[1] * distanceAlongRay - centre[1];
			const double distanceFromCentre = std::sqrt(offsetX * offsetX + offsetY * offsetY);
			if (distanceFromCentre > radius) continue;
			// Its depth along the view axis, in true proportions like the depth buffer's.
			const double viewDepth = distanceAlongRay * (rayDirection[0] * _forward[0] + rayDirection[1] * _forward[1]
				+ rayDirection[2] * ZScale * _forward[2]); // true depth
			if (std::abs(_depthBuffer[pixelY * width + pixelX] - viewDepth) > 3.0) continue; // only on the floor right there
			// Darker towards the middle: more of the pixels get the shade (dithered), each
			// darkened within its colour group.
			const float closeness = (float)(1.0 - distanceFromCentre / radius);
			if (closeness * density > dither(pixelX, pixelY) && pixelRow[pixelX])
			{
				pixelRow[pixelX] = (Uint8)((pixelRow[pixelX] & 0xF0) | std::min(15, (pixelRow[pixelX] & 15) + steps));
			}
		}
	}
}

/**
 * The topmost row of a sprite with anything coloured in it.
 * @param sprite The sprite (palette colours, 0 transparent).
 * @return The row; the sprite's height if it's all transparent.
 */
static int topColouredRow(const Surface *sprite)
{
	const SDL_Surface *surface = const_cast<Surface*>(sprite)->getSurface(); // read only
	for (int row = 0; row < surface->h; ++row)
	{
		const Uint8 *pixels = (const Uint8 *)surface->pixels + row * surface->pitch;
		for (int column = 0; column < surface->w; ++column)
		{
			if (pixels[column]) return row;
		}
	}
	return surface->h;
}

/**
 * The bottom row of a sprite with anything coloured in it.
 * @param sprite The sprite (palette colours, 0 transparent).
 * @return The row; -1 if it's all transparent.
 */
static int bottomColouredRow(const Surface *sprite)
{
	const SDL_Surface *surface = const_cast<Surface*>(sprite)->getSurface(); // read only
	for (int row = surface->h - 1; row >= 0; --row)
	{
		const Uint8 *pixels = (const Uint8 *)surface->pixels + row * surface->pitch;
		for (int column = 0; column < surface->w; ++column)
		{
			if (pixels[column]) return row;
		}
	}
	return -1;
}

/**
 * Draws the top item lying on each discovered tile (weapons, corpses, ...) as a sprite on the
 * floor, lit by the game's light just above it, at the units' size (setting "Unit size").
 * Something lying on a tall object (a shelf, a crate, the top of stairs) is right under the
 * floor above, if there is one: the part of it that stands up from the floor is squashed to fit
 * under that floor, a voxel short of it, or it poked up through the floor into the room above.
 * The rows lying on the floor rise with the ground in front (a step, a slope; see lyingRise).
 * A big unit's corpse lies in parts, one on each of its tiles (the armor's corpse items, part
 * y * 2 + x on the tile x, y from its corner, as the game drops them): while all four are the top
 * items of their tiles, they're drawn as one picture, put together as the map draws them, laid at
 * the corner the four tiles share and scaled round it (a player's report: a 2x2 corpse looked
 * chopped to pieces, each part shrunk round its own tile and standing on its own).
 */
void FirstPersonView::drawFloorItems()
{
	ItemSprite itemSprite(_spriteBuffer, _game->getMod(), _save, _save->getAnimFrame()); // on the game's animation counter, as the map draws
	const int tileCount = _save->getMapSizeXYZ();
	// The items' size: the units' (setting "Unit size"), so a unit going down doesn't grow into
	// a bigger corpse, and what lies by it keeps to the same scale.
	const double itemScale = std::clamp(Options::fpsUnitSize, 80, 100) / 100.0;
	// The tiles whose item went into a whole big corpse already.
	std::vector<int> partsDrawn;
	// A big unit's corpse part lying on a tile: the corner of the 2x2 corpse it's part of, when
	// all four parts lie there as the top items, in their places (the same unit's, each the
	// armor's corpse item for its place).
	auto wholeCorpseCorner = [&](const Tile *tile, const BattleItem *item, Position &corner) {
		const BattleUnit *unit = item->getUnit();
		if (!unit || item->getRules()->getBattleType() != BT_CORPSE || unit->getArmor()->getSize() != 2) return false;
		const auto &parts = unit->getArmor()->getCorpseBattlescape();
		if (parts.size() < 4) return false;
		const Position at = tile->getPosition();
		for (const Position &candidate : { at, at - Position(1, 0, 0), at - Position(0, 1, 0), at - Position(1, 1, 0) })
		{
			bool whole = true;
			for (int part = 0; part < 4 && whole; ++part)
			{
				const Tile *partTile = _save->getTile(candidate + Position(part % 2, part / 2, 0));
				const BattleItem *partItem = partTile ? const_cast<Tile*>(partTile)->getTopItem() : nullptr;
				whole = partItem && partItem->getUnit() == unit && partItem->getRules() == parts[part];
			}
			if (whole)
			{
				corner = candidate;
				return true;
			}
		}
		return false;
	};
	// The items are recorded first, then drawn on all the view's threads at once (see
	// replayDrawList); each one's frame is composed in the sprite buffer and kept till then.
	int framesUsed = 0;
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		// Only tiles with something on them, that have been seen.
		Tile *tile = _save->getTile(tileIndex);
		if (tile->getInventory()->empty() || !_tileViews[tileIndex].discovered
			|| std::find(partsDrawn.begin(), partsDrawn.end(), tileIndex) != partsDrawn.end())
		{
			continue;
		}
		BattleItem *item = tile->getTopItem();
		if (!item)
		{
			continue;
		}
		// The tiles it lies on: its own, or a whole big corpse's four.
		Position corner;
		const bool wholeCorpse = wholeCorpseCorner(tile, item, corner);
		std::vector<Tile*> tilesUnder = { tile };
		if (wholeCorpse)
		{
			tilesUnder.clear();
			for (int part = 0; part < 4; ++part)
			{
				Tile *partTile = _save->getTile(corner + Position(part % 2, part / 2, 0));
				tilesUnder.push_back(partTile);
				partsDrawn.push_back(_save->getTileIndex(partTile->getPosition()));
			}
		}
		// Where it lies: the middle of the tile's floor, or the corner a big corpse's tiles share,
		// on the highest of their floors; skip it if it's behind the camera or far off the sides.
		const Position tilePosition = tile->getPosition();
		double ground[3] = { tilePosition.x * 16.0 + 8.0, tilePosition.y * 16.0 + 8.0, tilePosition.z * 24.0 - tile->getTerrainLevel() };
		if (wholeCorpse)
		{
			ground[0] = corner.x * 16.0 + 16.0;
			ground[1] = corner.y * 16.0 + 16.0;
			for (const Tile *partTile : tilesUnder) ground[2] = std::max(ground[2], corner.z * 24.0 - partTile->getTerrainLevel());
		}
		double screenX, screenY, depth;
		if (!projectToScreen(ground, screenX, screenY, depth) || screenX < -200 || screenX > getWidth() + 200)
		{
			continue;
		}
		// Compose the item's floor sprite in the buffer (unlit) and lay it on the floor's surface:
		// the art below the middle of the floor's diamond lies flat, the rest stands. A big corpse's
		// parts go where the map draws them, like the 2x2 tiles they cover, back to front, and its
		// middle is the corner the four share.
		_spriteBuffer->clear();
		const int shade = (int)std::lround(std::clamp(lightAt((float)ground[0], (float)ground[1], (float)ground[2] + 4.0f), 0.0f, 15.0f));
		if (wholeCorpse)
		{
			for (int part = 0; part < 4; ++part)
			{
				const int x = part % 2, y = part / 2;
				itemSprite.draw(tilesUnder[part]->getTopItem(), 16 + (x - y) * 16, 8 + (x + y) * 8, 0);
			}
		}
		else
		{
			itemSprite.draw(item, 16, 8, 0);
		}
		DrawStep step;
		step.kind = DrawStep::Sprite;
		step.sprite.sprite = keepComposedFrame(framesUsed);
		step.sprite.anchorX = 32;
		step.sprite.anchorY = wholeCorpse ? 48 : 40;
		step.sprite.world[0] = ground[0];
		step.sprite.world[1] = ground[1];
		// On stairs and slopes (a player: items there, seen from the side, sank half into them;
		// the owner: "just on top of stairs at half height") it lies level at the height of the
		// ground under its middle (the plane through the ground round it, on the steps' edges:
		// see groundPlane; failing that, the floor's top, see standingHeight), drawn over the
		// steps round it: the depth test gives way by as much as the ground rises within its
		// reach, up to SlopeItemDepthBiasMax voxels. (Only where a tile under it has a terrain level,
		// as stairs, ramps and slopes do: a flat floor's top is simply its slab's; and not under a
		// floor: a corpse on a shelf below a terrace, lifted onto the shelf's edge and drawn over
		// what's round it, showed through the terrace's floor. A stairwell is open above.)
		const double planeReach = wholeCorpse ? GroundPlaneReachCorpse : GroundPlaneReach;
		double planeHeight = 0.0, planeSlope[2] = { 0.0, 0.0 };
		bool raised = false, underFloor = false;
		for (Tile *under : tilesUnder)
		{
			raised = raised || under->getTerrainLevel() != 0;
			const Tile *aboveTile = _save->getAboveTile(under);
			underFloor = underFloor || (aboveTile && !aboveTile->hasNoFloor());
		}
		const bool onPlane = raised && !underFloor && groundPlane(ground, planeReach, planeHeight, planeSlope);
		step.sprite.world[2] = onPlane ? planeHeight : standingHeight(ground);
		step.sprite.depthBias = std::min(std::hypot(planeSlope[0], planeSlope[1]) * planeReach, SlopeItemDepthBiasMax);
		step.sprite.shade = shade;
		step.sprite.scale = itemScale;
		step.sprite.pose = SpritePose::Lying;
		// An unconscious unit lying in the pile (any of a big corpse's tiles): its picture can be
		// pointed at (a medi-kit's patient).
		for (Tile *under : tilesUnder)
		{
			for (BattleItem *pileItem : *under->getInventory())
			{
				if (!step.sprite.body && pileItem->getUnit() && pileItem->getUnit()->getStatus() == STATUS_UNCONSCIOUS)
				{
					step.sprite.body = pileItem->getUnit();
				}
			}
		}
		// The ground in front rising (a step, a slope): the lying rows rise with it, within their reach.
		const int lyingRows = bottomColouredRow(step.sprite.sprite) - step.sprite.anchorY + 1;
		if (lyingRows > 0)
		{
			step.sprite.lyingSlope = lyingRise(step.sprite.world, lyingRows * LyingRowVoxels * itemScale);
		}
		// Room to stand up in under the floor above (its slab starts at the next level's foot),
		// against the sprite's coloured rows above the anchor, a voxel high each at full size.
		double headroom = 1e9;
		for (Tile *under : tilesUnder)
		{
			const Tile *aboveTile = _save->getAboveTile(under);
			if (aboveTile && !aboveTile->hasNoFloor())
			{
				headroom = std::min(headroom, (tilePosition.z + 1) * 24.0 - 1.0 - step.sprite.world[2]);
			}
		}
		const int standingRows = step.sprite.anchorY - topColouredRow(step.sprite.sprite);
		const double standingVoxels = standingRows * itemScale;
		if (headroom < 1e9 && standingRows > 0 && standingVoxels > headroom)
		{
			step.sprite.standingSquash = std::max(0.05, headroom) / standingVoxels;
		}
		useSmoothVersion(step.sprite);
		_drawList.push_back(step);
	}
	// Then draw them all.
	replayDrawList();
}

/**
 * Keeps what's composed in the sprite buffer (a unit's frame, an item's) for a sprite recorded
 * to be drawn later (see replayDrawList): copies it into the next free composed frame, made the
 * first time it's needed and kept for the next frames.
 * @param framesUsed How many composed frames the stage uses so far; counts this one.
 * @return The frame holding the copy.
 */
Surface *FirstPersonView::keepComposedFrame(int &framesUsed)
{
	if (framesUsed == (int)_composedFrames.size())
	{
		_composedFrames.push_back(new Surface(SpriteBufferSize, SpriteBufferSize));
	}
	Surface *frame = _composedFrames[framesUsed++];
	const SDL_Surface *composed = _spriteBuffer->getSurface();
	SDL_Surface *kept = frame->getSurface();
	for (int y = 0; y < SpriteBufferSize; ++y)
	{
		std::copy_n((const Uint8 *)composed->pixels + y * composed->pitch, SpriteBufferSize, (Uint8 *)kept->pixels + y * kept->pitch);
	}
	return frame;
}

/**
 * Draws a sprite part way through dissolving into the next animation frame (ordered dither),
 * so animations such as smoke, fire and explosions flow instead of stepping. The two frames
 * are mixed pixel by pixel into a buffer with a 4x4 dither pattern, more of the next frame the
 * further the blend, and the shapes meet half way: both frames are stretched to the size in
 * between, around the base they rise from. Close up both frames are upscaled first.
 * @param from The frame showing now.
 * @param to The next frame (without it, or of another size, `from` is drawn alone).
 * @param blend How far into the change: 0 all `from` .. 1 all `to`.
 * @param anchorX The frames' pixel column that stands at the point.
 * @param anchorY The frames' pixel row that stands at the point.
 * @param world The point, in voxels.
 * @param depthBias How much nearer than the point it counts for the depth test, in voxels.
 * @param shade Palette shades to darken it by.
 */
void FirstPersonView::drawSpriteBlend(const Surface *from, const Surface *to, double blend, int anchorX, int anchorY, const double world[3],
	double depthBias, int shade)
{
	if (!from)
	{
		return;
	}
	// Nothing to blend with: just the frame.
	if (!to || blend <= 0.0 || to->getWidth() != from->getWidth() || to->getHeight() != from->getHeight())
	{
		drawSpriteSmooth(from, anchorX, anchorY, world, depthBias, shade);
		return;
	}
	// Close up both frames are upscaled first (the boxes are measured on the originals, then
	// scaled up with them: each original pixel becomes factor x factor pixels).
	SpriteBox fromBox = spriteBox(from), toBox = spriteBox(to);
	double scale = 1.0;
	const Surface *bigFrom = upscaleHere(world, 1.0) ? upscaled(from) : nullptr;
	const Surface *bigTo = bigFrom ? upscaled(to) : nullptr;
	if (bigFrom && bigTo)
	{
		const int factor = UpscaleFactor;
		from = bigFrom;
		to = bigTo;
		anchorX *= factor;
		anchorY *= factor;
		scale = 1.0 / factor;
		for (SpriteBox *box : { &fromBox, &toBox })
		{
			box->x0 *= factor; box->y0 *= factor;
			box->x1 = box->x1 * factor + factor - 1; box->y1 = box->y1 * factor + factor - 1;
		}
	}
	// A buffer of the frames' size to mix them in (one is kept per size and reused).
	const int width = from->getWidth(), height = from->getHeight();
	Surface *buffer = nullptr;
	for (Surface *candidate : _blendBuffers)
	{
		if (candidate->getWidth() == width && candidate->getHeight() == height) buffer = candidate;
	}
	if (!buffer)
	{
		buffer = new Surface(width, height);
		_blendBuffers.push_back(buffer);
	}
	// A 4x4 ordered dither in sprite space: the pattern travels with the sprite, so it doesn't
	// crawl. (The same Bayer matrix as the shared one, in whole sixteenths.) Pixels whose level
	// is below the threshold show the next frame.
	static const int bayerLevels[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	const int threshold = (int)(blend * 16.0);
	// And the shapes meet half way: both frames stretched to the size in between, around the base
	// they rise from, so a growing explosion is one shape growing, not two ghosting into each other.
	const SDL_Surface *fromSurface = const_cast<Surface*>(from)->getSurface(), // read only
		*toSurface = const_cast<Surface*>(to)->getSurface();
	SDL_Surface *outSurface = buffer->getSurface();
	auto pixel = [](const SDL_Surface *surface, int x, int y) { return ((const Uint8 *)surface->pixels)[y * surface->pitch + x]; };
	// An empty frame has no shape to stretch: just dither the two frames together as they are.
	if (!fromBox.valid || !toBox.valid)
	{
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
				((Uint8 *)outSurface->pixels)[y * outSurface->pitch + x] = bayerLevels[y & 3][x & 3] < threshold
					? pixel(toSurface, x, y) : pixel(fromSurface, x, y);
		drawSprite(buffer, anchorX, anchorY, world, depthBias, shade, scale);
		return;
	}
	// The size in between (the boxes' sizes blended), and the base each shape rises from: the
	// middle of its box's bottom edge, blended too.
	const double progress = blend;
	const double fromBoxWidth = fromBox.x1 - fromBox.x0 + 1.0, fromBoxHeight = fromBox.y1 - fromBox.y0 + 1.0;
	const double toBoxWidth = toBox.x1 - toBox.x0 + 1.0, toBoxHeight = toBox.y1 - toBox.y0 + 1.0;
	const double midWidth = fromBoxWidth + (toBoxWidth - fromBoxWidth) * progress,
		midHeight = fromBoxHeight + (toBoxHeight - fromBoxHeight) * progress;
	const double fromBase[2] = { (fromBox.x0 + fromBox.x1 + 1) / 2.0, fromBox.y1 + 1.0 },
		toBase[2] = { (toBox.x0 + toBox.x1 + 1) / 2.0, toBox.y1 + 1.0 };
	const double midBase[2] = { fromBase[0] + (toBase[0] - fromBase[0]) * progress,
		fromBase[1] + (toBase[1] - fromBase[1]) * progress };
	// Where each output column and row reads from in either frame (-1: outside it); only the box
	// the stretched shapes can reach is worked out, the rest of the buffer is cleared.
	_blendColumns.resize(4 * (size_t)std::max(width, height));
	int *fromColumns = _blendColumns.data(), *toColumns = fromColumns + width,
		*fromRows = toColumns + width, *toRows = fromRows + height;
	// The source pixel for an output pixel: the output's base point maps onto the frame's own
	// base, and distances from it shrink by the stretch (the size in between / the frame's).
	auto source = [](double frameBase, double outputAt, double outputBase, double stretch, int length) {
		const int sourceIndex = (int)std::floor(frameBase + (outputAt + 0.5 - outputBase) / stretch);
		return sourceIndex >= 0 && sourceIndex < length ? sourceIndex : -1;
	};
	// The columns and rows, and the output box (left..right, top..bottom) that shows either shape.
	int left = width, right = -1, top = height, bottom = -1;
	for (int x = 0; x < width; ++x)
	{
		fromColumns[x] = source(fromBase[0], x, midBase[0], midWidth / fromBoxWidth, width);
		toColumns[x] = source(toBase[0], x, midBase[0], midWidth / toBoxWidth, width);
		if ((fromColumns[x] >= fromBox.x0 && fromColumns[x] <= fromBox.x1) || (toColumns[x] >= toBox.x0 && toColumns[x] <= toBox.x1))
		{
			left = std::min(left, x);
			right = x;
		}
	}
	for (int y = 0; y < height; ++y)
	{
		fromRows[y] = source(fromBase[1], y, midBase[1], midHeight / fromBoxHeight, height);
		toRows[y] = source(toBase[1], y, midBase[1], midHeight / toBoxHeight, height);
		if ((fromRows[y] >= fromBox.y0 && fromRows[y] <= fromBox.y1) || (toRows[y] >= toBox.y0 && toRows[y] <= toBox.y1))
		{
			top = std::min(top, y);
			bottom = y;
		}
	}
	// Fill the buffer row by row: transparent outside the box, and inside it each pixel from
	// one frame or the other by the dither pattern.
	for (int y = 0; y < height; ++y)
	{
		Uint8 *outRow = (Uint8 *)outSurface->pixels + y * outSurface->pitch;
		if (y < top || y > bottom)
		{
			std::fill(outRow, outRow + width, (Uint8)0);
			continue;
		}
		std::fill(outRow, outRow + std::max(0, left), (Uint8)0);
		if (right + 1 < width) std::fill(outRow + right + 1, outRow + width, (Uint8)0);
		const Uint8 *fromLine = fromRows[y] >= 0 ? (const Uint8 *)fromSurface->pixels + fromRows[y] * fromSurface->pitch : nullptr;
		const Uint8 *toLine = toRows[y] >= 0 ? (const Uint8 *)toSurface->pixels + toRows[y] * toSurface->pitch : nullptr;
		const int *ditherRow = bayerLevels[y & 3];
		for (int x = left; x <= right; ++x)
		{
			if (ditherRow[x & 3] < threshold) outRow[x] = toLine && toColumns[x] >= 0 ? toLine[toColumns[x]] : 0;
			else outRow[x] = fromLine && fromColumns[x] >= 0 ? fromLine[fromColumns[x]] : 0;
		}
	}
	drawSprite(buffer, anchorX, anchorY, world, depthBias, shade, scale);
}

/**
 * The box round a sprite frame's pixels: the first and last columns and rows that have any
 * (inclusive), or not valid for an empty frame. Measured once per frame, then remembered.
 */
const FirstPersonView::SpriteBox &FirstPersonView::spriteBox(const Surface *sprite)
{
	auto found = _spriteBoxes.find(sprite);
	if (found != _spriteBoxes.end())
	{
		return found->second;
	}
	// Grow the box around every opaque pixel (colour 0 is transparent).
	SpriteBox box = { sprite->getWidth(), sprite->getHeight(), -1, -1, false };
	for (int y = 0; y < sprite->getHeight(); ++y)
	{
		for (int x = 0; x < sprite->getWidth(); ++x)
		{
			if (!sprite->getPixel(x, y)) continue;
			box.x0 = std::min(box.x0, x); box.y0 = std::min(box.y0, y);
			box.x1 = std::max(box.x1, x); box.y1 = std::max(box.y1, y);
			box.valid = true;
		}
	}
	return _spriteBoxes.emplace(sprite, box).first->second;
}

/**
 * The discreet map edge (setting): instead of the tape round the map, small red dots on the
 * floor along its edge, every EdgeDotSpacing voxels, just inside it, on the ground floor's top.
 * Each is a little square that keeps its size in the world (at least two pixels, so it still
 * shows far away), hidden by what stands in front of it like anything else, and fading into
 * the haze in the distance.
 */
void FirstPersonView::drawEdgeDots()
{
	if (!Options::fpsDiscreetEdge)
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const float mapX = _save->getMapSizeX() * 16.0f, mapY = _save->getMapSizeY() * 16.0f;
	const double focalLength = (width / 2.0) / _halfFovTangent;
	// One dot: projected, sized by its distance, drawn where nothing nearer is.
	auto dot = [&](float x, float y) {
		const double point[3] = { x, y, GroundTop };
		double screenX, screenY, depth;
		if (!projectToScreen(point, screenX, screenY, depth) || depth > _maxDistance) return;
		const int size = std::max(2, (int)std::lround(EdgeDotSize * focalLength / depth));
		const float haze = smoothstep(_maxDistance * 0.3f, _maxDistance, (float)depth);
		const int left = (int)std::lround(screenX - size / 2.0), top = (int)std::lround(screenY - size / 2.0);
		for (int pixelY = std::max(0, top); pixelY < std::min(height, top + size); ++pixelY)
		{
			for (int pixelX = std::max(0, left); pixelX < std::min(width, left + size); ++pixelX)
			{
				if ((float)depth >= _depthBuffer[pixelY * width + pixelX] + 1.0f || haze > dither(pixelX, pixelY)) continue;
				*getRaw(pixelX, pixelY) = shadeColor(_colors.edgeDot, (int)(_colors.ambient * 0.5f));
			}
		}
	};
	// Along the four edges, a dot every EdgeDotSpacing voxels, half a spacing from each corner.
	for (float along = EdgeDotSpacing / 2.0f; along < mapX; along += EdgeDotSpacing)
	{
		dot(along, EdgeDotInset);
		dot(along, mapY - EdgeDotInset);
	}
	for (float along = EdgeDotSpacing / 2.0f; along < mapY; along += EdgeDotSpacing)
	{
		dot(EdgeDotInset, along);
		dot(mapX - EdgeDotInset, along);
	}
}

/**
 * Fire on discovered tiles, animated like on the isometric map: the same sprites from SMOKE.PCK
 * (its first four frames), with the frames dissolving into each other. The smoke is drawn
 * see-through after everything else (drawSeeThroughSmoke), and with Modern solid where the
 * soldier's sight runs out in it, by the rays (see SmokeTrace).
 */
void FirstPersonView::drawFire()
{
	SurfaceSet *smokeSprites = _game->getMod()->getSurfaceSet("SMOKE.PCK", false);
	if (!smokeSprites)
	{
		return;
	}
	// The four frames last 200 ms each like on the map; each dissolves into the next meanwhile.
	const Uint32 now = SDL_GetTicks();
	const int halfAnimFrame = (now / 200) % 4; // the map's name for it: a frame every second animation tick
	const double blend = (now % 200) / 200.0;
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		// Only burning tiles (a burning tile has smoke too) that have been seen.
		Tile *tile = _save->getTile(tileIndex);
		if (!tile->getSmoke() || !tile->getFire() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		// Fire is the set's first four frames, unshaded (it shines).
		const int firstFrame = 0, shade = 0;
		// Each tile is a little out of step with the others (its animation offset), like on
		// the map; the frame shown dissolves into the next one, on the middle of the floor.
		const int phase = (halfAnimFrame + tile->getAnimationOffset()) % 4;
		const Position tilePosition = tile->getPosition();
		const double ground[3] = { tilePosition.x * 16.0 + 8.0, tilePosition.y * 16.0 + 8.0,
			tilePosition.z * 24.0 - tile->getTerrainLevel() };
		drawSpriteBlend(smokeSprites->getFrame(firstFrame + phase), smokeSprites->getFrame(firstFrame + (phase + 1) % 4), blend, 16, 32,
			ground, 2.0, shade);
	}
}

/**
 * The first of the four frames of SMOKE.PCK the map shows for a density of smoke (1 .. 15 or
 * so), as it picks them for the mod's smoke layout; under the sea, its own frames.
 * @param smoke The tile's smoke.
 * @return The frame number.
 */
int FirstPersonView::firstSmokeFrame(int smoke) const
{
	int firstFrame = _save->getDepth() > 0 ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
	if (Mod::EXTENDED_SMOKE_OFFSET == 0)
		firstFrame += int(std::floor((smoke / 6.0) - 0.1));
	else if (Mod::EXTENDED_SMOKE_OFFSET == 1)
		firstFrame += int(std::floor((smoke / 6.0) - 0.1)) * 4;
	else
		firstFrame += (smoke - 1) / 5 * 4;
	return firstFrame;
}

/**
 * Whether a smoky tile is on its cloud's outer shell: one of the four tiles beside it (on its
 * level, across a side) has no smoke the view knows of, or is off the map. Original smoke
 * stacks such a tile less where the soldier can't see through it (see drawSeeThroughSmoke),
 * so the cloud's outline stays loose.
 * @param tilePosition The tile.
 * @return True if it's on the shell.
 */
bool FirstPersonView::smokeShell(const Position &tilePosition) const
{
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	static const int sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	for (const auto &side : sides)
	{
		const int x = tilePosition.x + side[0], y = tilePosition.y + side[1];
		if (x < 0 || y < 0 || x >= sizeX || y >= sizeY || !_tileViews[(tilePosition.z * sizeY + y) * sizeX + x].smoke)
		{
			return true;
		}
	}
	return false;
}

/**
 * The colour of the game's smoke, from its frames for the thickest smoke: the colours of their
 * dots mixed and turned most of the way to grey (as the see-through smoke's are, see
 * softSmokeFrame), so the wall of smoke the rays draw matches the see-through smoke over it.
 * Grey if the mod has no smoke frames.
 * @param rgb Set to the colour (0 .. 255 per channel).
 */
void FirstPersonView::smokeRgb(float rgb[3]) const
{
	rgb[0] = rgb[1] = rgb[2] = 130.0f;
	SurfaceSet *smokeSprites = _game->getMod()->getSurfaceSet("SMOKE.PCK", false);
	if (!smokeSprites || !_save)
	{
		return;
	}
	const SDL_Color *palette = getPalette();
	double sum[3] = { 0.0, 0.0, 0.0 };
	int dots = 0;
	const int firstFrame = firstSmokeFrame(15);
	for (int frameNumber = firstFrame; frameNumber < firstFrame + 4; ++frameNumber)
	{
		Surface *frame = smokeSprites->getFrame(frameNumber);
		if (!frame) continue;
		const SDL_Surface *frameSurface = frame->getSurface();
		for (int y = 0; y < frame->getHeight(); ++y)
		{
			for (int x = 0; x < frame->getWidth(); ++x)
			{
				const Uint8 color = ((const Uint8 *)frameSurface->pixels)[y * frameSurface->pitch + x];
				if (!color) continue;
				sum[0] += palette[color].r; sum[1] += palette[color].g; sum[2] += palette[color].b;
				++dots;
			}
		}
	}
	if (dots)
	{
		const float mixed[3] = { (float)(sum[0] / dots), (float)(sum[1] / dots), (float)(sum[2] / dots) };
		const float grey = mixed[0] * 0.3f + mixed[1] * 0.59f + mixed[2] * 0.11f;
		for (int channel = 0; channel < 3; ++channel) rgb[channel] = grey + (mixed[channel] - grey) * SmokeColorKept;
	}
}

/**
 * Makes a frame of the game's smoke see-through. The original smoke is a stipple: scattered
 * dots, more of them where the smoke is thicker, standing in for see-through smoke on the
 * isometric map. Here the frame is made UpscaleFactor times bigger and the dots are blurred over
 * it (a Gaussian, about a pixel and a quarter of the original wide): how densely they lie becomes
 * how opaque the smoke is, their colours mixed become its colour (turned most of the way to
 * grey, as mixed dots of the palette's browns and greys would otherwise come out in patches of
 * colour). So the smoke keeps the game's
 * shapes, colours and animation, but soft, and truly see-through. A mod's smoke drawn solid
 * becomes evenly opaque, with soft edges. Made once per frame (and again for another palette).
 * @param frame The smoke frame (in the view's palette).
 * @return The see-through frame: colours (0 where there's no smoke) and the opacity of each
 *         pixel in see-through steps (0 .. SeeThroughLevels).
 */
const FirstPersonView::SoftSmoke &FirstPersonView::softSmokeFrame(const Surface *frame)
{
	// The colours are the palette's: made again when it changes.
	if (_softSmokePalette != _colors.paletteHash)
	{
		for (auto &entry : _softSmoke) delete entry.second.colors;
		_softSmoke.clear();
		for (auto &entry : _stippleSmoke) delete entry.second.colors;
		_stippleSmoke.clear();
		for (auto &entry : _softSmokeBlends) delete entry.second.colors;
		_softSmokeBlends.clear();
		_softSmokePalette = _colors.paletteHash;
	}
	auto made = _softSmoke.find(frame);
	if (made != _softSmoke.end())
	{
		return made->second;
	}

	// The original's dots: where they are (1) and their colours, as RGB weighted by that.
	const int width = frame->getWidth(), height = frame->getHeight();
	const int bigWidth = width * UpscaleFactor, bigHeight = height * UpscaleFactor;
	const SDL_Color *palette = getPalette();
	const SDL_Surface *frameSurface = const_cast<Surface*>(frame)->getSurface(); // read only
	std::vector<float> dots(width * height * 4, 0.0f); // per pixel: covered, then red, green, blue times that
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const Uint8 color = ((const Uint8 *)frameSurface->pixels)[y * frameSurface->pitch + x];
			if (!color) continue;
			float *dot = &dots[(y * width + x) * 4];
			dot[0] = 1.0f; dot[1] = palette[color].r; dot[2] = palette[color].g; dot[3] = palette[color].b;
		}
	}
	// The blur's weights for each big pixel along one axis: the original pixels around its
	// centre, by a Gaussian of their distance (sigma 1.25 original pixels), adding up to 1.
	// (The same along both axes, so the blur goes across, then down.)
	const int Taps = 9; // original pixels each big pixel takes from, round its centre
	auto blurWeights = [](int bigCount, std::vector<int> &firstPixel, std::vector<float> &weights) {
		firstPixel.resize(bigCount);
		weights.assign(bigCount * Taps, 0.0f);
		for (int big = 0; big < bigCount; ++big)
		{
			const float centre = (big + 0.5f) / UpscaleFactor - 0.5f; // in original pixels
			firstPixel[big] = (int)std::floor(centre) - Taps / 2;
			float total = 0.0f;
			for (int tap = 0; tap < Taps; ++tap)
			{
				const float distance = firstPixel[big] + tap - centre;
				total += weights[big * Taps + tap] = std::exp(-distance * distance / (2.0f * 1.25f * 1.25f));
			}
			for (int tap = 0; tap < Taps; ++tap) weights[big * Taps + tap] /= total;
		}
	};
	std::vector<int> firstColumn, firstRow;
	std::vector<float> columnWeights, rowWeights;
	blurWeights(bigWidth, firstColumn, columnWeights);
	blurWeights(bigHeight, firstRow, rowWeights);
	// Across: each original row blurred out to the big width.
	std::vector<float> across(height * bigWidth * 4, 0.0f);
	for (int y = 0; y < height; ++y)
	{
		for (int bigX = 0; bigX < bigWidth; ++bigX)
		{
			float *out = &across[(y * bigWidth + bigX) * 4];
			for (int tap = 0; tap < Taps; ++tap)
			{
				const int x = firstColumn[bigX] + tap;
				if (x < 0 || x >= width) continue;
				const float weight = columnWeights[bigX * Taps + tap];
				const float *dot = &dots[(y * width + x) * 4];
				for (int channel = 0; channel < 4; ++channel) out[channel] += dot[channel] * weight;
			}
		}
	}
	// Down: each big pixel from the rows round it. The opacity is how densely the dots lie
	// (more than twice that, as a stipple covers less than half the pixels where it means solid
	// smoke), the colour their colours mixed, back in the palette.
	SoftSmoke &soft = _softSmoke[frame];
	soft.colors = new Surface(bigWidth, bigHeight);
	soft.opacity.assign(bigWidth * bigHeight, 0);
	SDL_Surface *colorSurface = soft.colors->getSurface();
	for (int bigY = 0; bigY < bigHeight; ++bigY)
	{
		Uint8 *colorRow = (Uint8 *)colorSurface->pixels + bigY * colorSurface->pitch;
		for (int bigX = 0; bigX < bigWidth; ++bigX)
		{
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int tap = 0; tap < Taps; ++tap)
			{
				const int y = firstRow[bigY] + tap;
				if (y < 0 || y >= height) continue;
				const float weight = rowWeights[bigY * Taps + tap];
				const float *value = &across[(y * bigWidth + bigX) * 4];
				for (int channel = 0; channel < 4; ++channel) sum[channel] += value[channel] * weight;
			}
			const int opacity = (int)std::lround(std::min(1.0f, sum[0] * 2.6f) * SeeThroughLevels);
			colorRow[bigX] = 0;
			if (opacity <= 0) continue;
			const float mixed[3] = { sum[1] / sum[0], sum[2] / sum[0], sum[3] / sum[0] };
			const float grey = mixed[0] * 0.3f + mixed[1] * 0.59f + mixed[2] * 0.11f;
			auto toFiveBits = [&](int channel) { return std::clamp((int)std::lround((grey + (mixed[channel] - grey) * SmokeColorKept) * 31.0f / 255.0f), 0, 31); };
			colorRow[bigX] = _nearestRgb[(toFiveBits(0) << 10) | (toFiveBits(1) << 5) | toFiveBits(2)];
			soft.opacity[bigY * bigWidth + bigX] = (Uint8)opacity;
		}
	}
	return soft;
}

/**
 * A frame of the game's smoke as it is, for the setting "Smoke: Original": its dots in their own
 * colours, each one solid (opacity SeeThroughLevels) and the holes between them empty, UpscaleFactor
 * times bigger, so it's drawn as the soft frames are: with sprite upscaling on, the upscaler's
 * version (the dots rounded, as the solid smoke's are), each dot a block until that's ready. How
 * see-through it looks is left to the opacity of the whole frame (the smoke's density), and the
 * original's own stipple: thin smoke has few dots, thick smoke many. Made once per frame (and
 * again for another palette, with the soft ones; and once more when the upscaler's is ready).
 * @param frame The smoke frame (in the view's palette).
 * @return The frame: colours (0 where there's no dot) and opacities.
 */
const FirstPersonView::SoftSmoke &FirstPersonView::stippleSmokeFrame(const Surface *frame)
{
	// Made before, as good as it gets now: as it is. The blocks are made again once the
	// upscaler's version is ready (the frames between two of them go with the old ones).
	const Surface *smooth = Options::fpsSpriteUpscale > 0 ? upscaled(frame) : nullptr;
	auto made = _stippleSmoke.find(frame);
	if (made != _stippleSmoke.end())
	{
		if (made->second.smooth || !smooth)
		{
			return made->second;
		}
		delete made->second.colors;
		_stippleSmoke.erase(made);
		++_smokeUpgrades; // (see drawSeeThroughSmoke)
		for (auto blend = _softSmokeBlends.begin(); blend != _softSmokeBlends.end();)
		{
			if (std::get<3>(blend->first) && (std::get<0>(blend->first) == frame || std::get<1>(blend->first) == frame))
			{
				delete blend->second.colors;
				blend = _softSmokeBlends.erase(blend);
			}
			else ++blend;
		}
	}
	const int width = frame->getWidth(), height = frame->getHeight();
	const int bigWidth = width * UpscaleFactor, bigHeight = height * UpscaleFactor;
	SoftSmoke &stipple = _stippleSmoke[frame];
	stipple.colors = new Surface(bigWidth, bigHeight);
	stipple.opacity.assign(bigWidth * bigHeight, 0);
	stipple.smooth = smooth != nullptr;
	const SDL_Surface *source = const_cast<Surface*>(smooth ? smooth : frame)->getSurface(); // read only
	const int sourceScale = smooth ? 1 : UpscaleFactor; // big pixels per source pixel
	SDL_Surface *colorSurface = stipple.colors->getSurface();
	for (int bigY = 0; bigY < bigHeight; ++bigY)
	{
		const Uint8 *sourceRow = (const Uint8 *)source->pixels + (bigY / sourceScale) * source->pitch;
		Uint8 *colorRow = (Uint8 *)colorSurface->pixels + bigY * colorSurface->pitch;
		for (int bigX = 0; bigX < bigWidth; ++bigX)
		{
			const Uint8 color = sourceRow[bigX / sourceScale];
			colorRow[bigX] = color;
			stipple.opacity[bigY * bigWidth + bigX] = color ? SeeThroughLevels : 0;
		}
	}
	return stipple;
}

/**
 * Two see-through smoke frames part way into each other, made once for each step and kept: each
 * pixel's opacity runs evenly from the first frame's to the second's, and its colour from the
 * first's to the second's (through the see-through mix; where only one has smoke, its colour).
 * So as the step goes up, each pixel only ever changes one way: the smoke melts from one frame
 * into the next without a pixel blinking back and forth.
 * @param from The frame the smoke is changing from (of SMOKE.PCK).
 * @param to The frame it's changing into (of the same size), or null.
 * @param step How far: 0 (all `from`) .. SmokeBlendSteps - 1.
 * @param original The original frames' dots as they are (setting "Smoke: Original"), not made soft.
 * @return The see-through frame between them.
 */
const FirstPersonView::SoftSmoke &FirstPersonView::softSmokeBlend(const Surface *from, const Surface *to, int step, bool original)
{
	softSmokeFrame(from); // (any palette change is dealt with here, before the frames are taken)
	const SoftSmoke &softFrom = original ? stippleSmokeFrame(from) : softSmokeFrame(from);
	if (step <= 0 || !to || to->getWidth() != from->getWidth() || to->getHeight() != from->getHeight())
	{
		return softFrom;
	}
	const SoftSmoke &softTo = original ? stippleSmokeFrame(to) : softSmokeFrame(to);
	const auto key = std::make_tuple(from, to, step, original);
	auto made = _softSmokeBlends.find(key);
	if (made != _softSmokeBlends.end())
	{
		return made->second;
	}
	const int width = softFrom.colors->getWidth(), height = softFrom.colors->getHeight();
	SoftSmoke &blend = _softSmokeBlends[key];
	blend.colors = new Surface(width, height);
	blend.opacity.assign(width * height, 0);
	const SDL_Surface *fromSurface = softFrom.colors->getSurface(), *toSurface = softTo.colors->getSurface();
	SDL_Surface *blendSurface = blend.colors->getSurface();
	const int colorLevel = (step * SeeThroughLevels + SmokeBlendSteps / 2) / SmokeBlendSteps; // how far the colour has gone, in see-through steps
	for (int y = 0; y < height; ++y)
	{
		const Uint8 *fromRow = (const Uint8 *)fromSurface->pixels + y * fromSurface->pitch, *toRow = (const Uint8 *)toSurface->pixels + y * toSurface->pitch;
		Uint8 *blendRow = (Uint8 *)blendSurface->pixels + y * blendSurface->pitch;
		for (int x = 0; x < width; ++x)
		{
			const int index = y * width + x;
			blend.opacity[index] = (Uint8)((softFrom.opacity[index] * (SmokeBlendSteps - step) + softTo.opacity[index] * step + SmokeBlendSteps / 2) / SmokeBlendSteps);
			blendRow[x] = !fromRow[x] ? toRow[x] : !toRow[x] ? fromRow[x] : seeThrough(colorLevel, toRow[x], fromRow[x]);
			if (!blend.opacity[index]) blendRow[x] = 0;
		}
	}
	return blend;
}

/**
 * The sight of the soldier the view looks through, for smoke drawn as the game's sight rules have
 * it (setting "Smoke": Modern's wall, Original's stacked frames): the range in voxels, and how
 * much of the smoke and of the smoke over fire their eyes see through (the armor's heat vision,
 * in percent). The see-through smoke is drawn thinner by as much (a player: a scout drone's 50%
 * heat vision didn't look any better), down to SmokeSeenMin of itself (see drawSeeThroughSmoke).
 * @param sightVoxels Their sight range, in voxels.
 * @param throughSmoke How much of smoke they see through, in percent.
 * @param throughFire How much of smoke over fire they see through, in percent.
 */
void FirstPersonView::setSmokeSight(float sightVoxels, int throughSmoke, int throughFire)
{
	_smokeSightVoxels = sightVoxels;
	_smokeBlock = (100 - std::clamp(throughSmoke, 0, 100)) / 300.0f;
	_fireBlock = (100 - std::clamp(throughFire, 0, 100)) / 300.0f;
	_smokeSeen = std::max(SmokeSeenMin, (100 - std::clamp(throughSmoke, 0, 100)) / 100.0f);
}

/**
 * The smoke on discovered tiles drawn see-through (setting "Smoke"): with Modern, the game's own
 * smoke made soft (see softSmokeFrame), and the wisps rising off it; where the soldier's sight
 * runs out in the smoke, the rays have already drawn a wall of it (see SmokeTrace). With
 * Original, the original frames' own dots (see stippleSmokeFrame), more opaque the thicker the
 * smoke, and no wisps: one cloud on a tile where the soldier sees through the smoke, and where
 * they can't (by the same rules, see smokeSightUsedThrough), the tile's animation over and over,
 * each out of step and set off a little, so their dots fill each other's gaps and the cloud
 * hides what's behind it. Drawn after everything solid, farthest first, each laid
 * over what's behind it; units the soldier sees stay clear (see _unitVeil).
 * Each tile's smoke is the frame the map picks for its density, a little more opaque the
 * thicker it is; its frames melt into each other slowly (an eased cross-fade, nothing blinks),
 * each tile out of step with the others, a fifth bigger than on the map give or take a little
 * (so the clouds of neighbouring tiles overlap, and a cloud's edge isn't the tiles' square), set
 * off from the middle, and swaying and bobbing gently. A wisp is a small puff of the thinnest smoke that rises,
 * grows and thins out, fading in and away.
 */
void FirstPersonView::drawSeeThroughSmoke()
{
	SurfaceSet *smokeSprites = _game->getMod()->getSurfaceSet("SMOKE.PCK", false);
	if (!smokeSprites || _nearestRgb.empty())
	{
		return;
	}
	const bool original = FirstPerson::smokeMode() == 1;
	typedef FirstPersonParticles Particles;
	const double seconds = SDL_GetTicks() / 1000.0;
	// A frame that changes into the next over FrameSeconds, eased in and out (smoothstep).
	const double FrameSeconds = 0.8;
	auto easedBlend = [](double fraction) { return fraction * fraction * (3.0 - 2.0 * fraction); };

	// Everything to draw: a puff of smoke, its anchor in the world (its frames' anchor pixel),
	// the two frames it is between, how far, how opaque, how big and how lit.
	struct Puff { double depth; double world[3]; int anchorY; const Surface *from, *to; double blend; float opacity; double scale; int shade; };
	std::vector<Puff> puffs;
	auto addPuff = [&](const double world[3], int anchorY, int firstFrame, double frameTime, float opacity, double scale, int shade) {
		const int phase = ((int)std::floor(frameTime) % 4 + 4) % 4;
		Puff puff;
		puff.depth = (world[0] - _eye[0]) * _forward[0] + (world[1] - _eye[1]) * _forward[1] + (world[2] - _eye[2]) * ZScale * _forward[2];
		if (puff.depth < 1.0) return;
		std::copy(world, world + 3, puff.world);
		puff.anchorY = anchorY;
		puff.from = smokeSprites->getFrame(firstFrame + phase);
		puff.to = smokeSprites->getFrame(firstFrame + (phase + 1) % 4);
		puff.blend = easedBlend(frameTime - std::floor(frameTime));
		puff.opacity = opacity;
		puff.scale = scale;
		puff.shade = shade;
		if (puff.from) puffs.push_back(puff);
	};

	// Each smoky tile seen (a burning one shows its fire instead), within the view's reach.
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile->getSmoke() || tile->getFire() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		const Position tilePosition = tile->getPosition();
		const double ground[3] = { tilePosition.x * 16.0 + 8.0, tilePosition.y * 16.0 + 8.0, tilePosition.z * 24.0 - tile->getTerrainLevel() };
		if (std::abs(ground[0] - _eye[0]) > _maxDistance || std::abs(ground[1] - _eye[1]) > _maxDistance)
		{
			continue;
		}
		// The tile's own variations, from its position: four numbers 0 .. 1.
		Uint32 hash = (Uint32)tileIndex * 2654435761u;
		float variation[4];
		for (float &value : variation)
		{
			hash ^= hash >> 15; hash *= 2246822519u; hash ^= hash >> 13;
			value = (hash & 0xFFFF) / 65535.0f;
		}
		// Set off from the middle, swaying and bobbing slowly.
		const double world[3] = {
			ground[0] + (variation[0] - 0.5) * 4.0 + 0.8 * std::sin(seconds * 0.5 + variation[1] * 2.0 * Pi),
			ground[1] + (variation[1] - 0.5) * 4.0 + 0.8 * std::cos(seconds * 0.4 + variation[2] * 2.0 * Pi),
			ground[2] + 1.0 * std::sin(seconds * 0.7 + variation[3] * 2.0 * Pi) };
		// A little more opaque the thicker the smoke (a fresh smoke grenade makes about 15); the
		// original's dots more so, as their holes already let the view through. Thinner for eyes
		// with heat vision (see setSmokeSight).
		const float opacity = _smokeSeen * (original ? std::clamp(0.35f + tile->getSmoke() * 0.04f, 0.0f, 0.95f) : std::clamp(0.45f + tile->getSmoke() * 0.025f, 0.0f, 0.85f));
		const int firstFrame = firstSmokeFrame(tile->getSmoke());
		const double frameTime = seconds / FrameSeconds + tile->getAnimationOffset() + variation[2], scale = 1.14 + variation[3] * 0.12;
		addPuff(world, 32, firstFrame, frameTime, opacity, scale, _tileViews[tileIndex].light);
		// Original, where the soldier can't see through the smoke looking at the cloud's middle
		// (the sight runs out on the line through it and on): the cloud again on top, out of
		// step and set off a little round the tile's middle (a frame of the original's dots
		// covers only a tenth to a third of its picture), OriginalSmokeMaxLayers times in all
		// inside the cloud, solid where they overlap, and OriginalSmokeShellLayers times on its
		// outer shell (a tile next to one without smoke), so its outline stays loose and
		// smoke-like; where the smoke uses up most of the sight, a few times (the cloud's edge,
		// as soft as Modern's wall's); where the soldier sees through it, once. (With heat
		// vision the sight lasts longer, so fewer layers stack; where it runs out all the same,
		// the layers are as solid.)
		if (original)
		{
			const double middle[3] = { world[0], world[1], world[2] + 12.0 };
			const float sightUsed = smokeSightUsedThrough(middle);
			const int mostLayers = smokeShell(tilePosition) ? OriginalSmokeShellLayers : OriginalSmokeMaxLayers;
			const int layers = std::min(mostLayers, sightUsed >= 1.0f ? OriginalSmokeMaxLayers : sightUsed >= 0.7f ? 4 : sightUsed >= 0.4f ? 2 : 1);
			for (int layer = 1; layer < layers; ++layer)
			{
				const double angle = layer * 2.4; // the golden angle, so the layers spread evenly round the middle
				const double shifted[3] = { world[0] + 3.0 * std::cos(angle), world[1] + 3.0 * std::sin(angle), world[2] + (layer % 3 - 1) * 2.0 };
				addPuff(shifted, 32, firstFrame, frameTime + layer * 1.37, 1.0f, scale * (1.0 + layer * 0.03), _tileViews[tileIndex].light);
			}
		}
	}
	// The wisps: the thinnest smoke's frames, anchored at their middle, growing as they rise,
	// fading in over their first quarter and away over their last half.
	for (const auto &particle : _particles.all())
	{
		if (particle.kind != Particles::WISP || original) // (wisps left from Modern are dropped)
		{
			continue;
		}
		const float age = particle.age / particle.life; // 0 new .. 1 gone
		const float fade = std::min(std::clamp(age / 0.25f, 0.0f, 1.0f), std::clamp((1.0f - age) / 0.5f, 0.0f, 1.0f));
		const double world[3] = { particle.pos[0], particle.pos[1], particle.pos[2] };
		const int shade = (int)std::lround(std::clamp(lightAt(particle.pos[0], particle.pos[1], particle.pos[2]), 0.0f, 15.0f));
		addPuff(world, 20, firstSmokeFrame(1), seconds / (FrameSeconds * 1.5) + particle.size, _smokeSeen * 0.45f * fade * fade * (3.0f - 2.0f * fade),
			0.35 + 0.4 * age, shade);
	}

	// Farthest first, each over what's behind it: its two frames part way into each other, made
	// here (on this thread). An original frame upgraded to the upscaler's version on the way
	// drops the blends made with the old one, some of which may be in the list already: then the
	// list is made again (it happens once for each frame, ever).
	std::sort(puffs.begin(), puffs.end(), [](const Puff &a, const Puff &b) { return a.depth > b.depth; });
	unsigned upgradesBefore;
	do
	{
		upgradesBefore = _smokeUpgrades;
		_smokeDraws.clear();
		for (const Puff &puff : puffs)
		{
			const SoftSmoke &soft = softSmokeBlend(puff.from, puff.to, std::min(SmokeBlendSteps - 1, (int)(puff.blend * SmokeBlendSteps)), original);
			_smokeDraws.push_back({ &soft, { puff.world[0], puff.world[1], puff.world[2] }, puff.anchorY, puff.opacity, puff.scale, puff.shade });
		}
	} while (_smokeUpgrades != upgradesBefore);
	// Then drawn on all the view's threads, each a band of rows at a time (see drawSmokeBands).
	if (!_smokeDraws.empty())
	{
		runOnAllThreads(&FirstPersonView::drawSmokeBands);
	}
}

/**
 * Draws this frame's see-through smoke (made by drawSeeThroughSmoke) over bands of the view's
 * rows, taken one at a time from the shared counter until none are left: a job for all the
 * view's threads (see runOnAllThreads). Each band gets every puff, farthest first, so every
 * pixel comes out just as if one thread drew the puffs whole, one after another.
 */
void FirstPersonView::drawSmokeBands()
{
	const int bandCount = (getHeight() + BandRows - 1) / BandRows;
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		for (const SmokeDraw &smoke : _smokeDraws)
		{
			const SeeThroughSprite seeThroughSprite = { smoke.soft->opacity.data(), smoke.opacity };
			SpriteDraw draw;
			draw.sprite = smoke.soft->colors;
			draw.anchorX = 16 * UpscaleFactor;
			draw.anchorY = smoke.anchorY * UpscaleFactor;
			std::copy(smoke.world, smoke.world + 3, draw.world);
			draw.depthBias = 2.0;
			draw.shade = smoke.shade;
			draw.scale = smoke.scale / UpscaleFactor;
			draw.seeThrough = &seeThroughSprite;
			drawSprite(draw, band * BandRows, (band + 1) * BandRows);
		}
	}
}

/**
 * Shows which way the units in sight face, while the key for it is held (a player's report:
 * "Alt being able to show unit facing would be useful ... Some units are not sprited to display
 * facing"; the owner: a key of its own, a marker like the hitboxes): round each one's footprint,
 * flat at the bottom of its hitbox, a ring and an arrow out from it the way it faces, in the
 * colour of its side, drawn over everything (what the soldier knows of it, like the markers).
 */
void FirstPersonView::drawFacing()
{
	for (const FacingSpot &spot : _facingSpots)
	{
		// The ring: an octagon round the footprint, flat on the floor.
		float corners[8][3];
		for (int corner = 0; corner < 8; ++corner)
		{
			const double angle = (corner + 0.5) * Pi / 4.0;
			corners[corner][0] = (float)(spot.feet[0] + std::cos(angle) * spot.radius);
			corners[corner][1] = (float)(spot.feet[1] + std::sin(angle) * spot.radius);
			corners[corner][2] = (float)spot.feet[2];
		}
		for (int corner = 0; corner < 8; ++corner)
		{
			drawLine3D(corners[corner], corners[(corner + 1) % 8], spot.color, 1, 1.0f, 0.0f, 0, 1 << 30, false, true);
		}
		// The arrow: a triangle out from the ring the way the unit faces (the game's directions:
		// 0 north, clockwise; the view's angles from east, so a quarter turn back).
		const double facing = (spot.direction * 45.0 - 90.0) * Pi / 180.0;
		const double ahead[2] = { std::cos(facing), std::sin(facing) }, side[2] = { -ahead[1], ahead[0] };
		const float tip[3] = { (float)(spot.feet[0] + ahead[0] * (spot.radius + FacingArrowLength)), (float)(spot.feet[1] + ahead[1] * (spot.radius + FacingArrowLength)),
			(float)spot.feet[2] };
		float base[2][3];
		for (int end = 0; end < 2; ++end)
		{
			const double across = end ? FacingArrowWidth : -FacingArrowWidth;
			base[end][0] = (float)(spot.feet[0] + ahead[0] * spot.radius + side[0] * across);
			base[end][1] = (float)(spot.feet[1] + ahead[1] * spot.radius + side[1] * across);
			base[end][2] = (float)spot.feet[2];
		}
		drawLine3D(base[0], tip, spot.color, 2, 1.0f, 0.0f, 0, 1 << 30, false, true);
		drawLine3D(base[1], tip, spot.color, 2, 1.0f, 0.0f, 0, 1 << 30, false, true);
		drawLine3D(base[0], base[1], spot.color, 2, 1.0f, 0.0f, 0, 1 << 30, false, true);
	}
}

/**
 * Marks the visible enemies like the iso map's indicators do: a red arrow over the head with
 * the enemy's number, drawn over everything (it's what the soldier knows, not what's in view),
 * kept at the screen edge when the enemy is off to the side. The pixel art is drawn bigger on
 * big screens (the UI scale).
 */
void FirstPersonView::drawMarkers()
{
	const int width = getWidth(), height = getHeight();
	const int uiScale = std::max(1, _uiScale);
	int scaleOriginX = 0, scaleOriginY = 0; // the marker's point: the pixel art is drawn uiScale times bigger around it
	// Puts one pixel of the marker's art (unscaled coordinates on the view): a uiScale x uiScale
	// block, scaled out from the marker's point, clipped to the view.
	auto put = [&](int x, int y, Uint8 color) {
		const int pixelX = scaleOriginX + (x - scaleOriginX) * uiScale, pixelY = scaleOriginY + (y - scaleOriginY) * uiScale;
		for (int offsetY = 0; offsetY < uiScale; ++offsetY)
			for (int offsetX = 0; offsetX < uiScale; ++offsetX)
				if (pixelX + offsetX >= 0 && pixelY + offsetY >= 0 && pixelX + offsetX < width && pixelY + offsetY < height)
					*getRaw(pixelX + offsetX, pixelY + offsetY) = color;
	};
	for (const auto &spot : _markerSpots)
	{
		// Where the enemy's head is on the view (no marker if it's behind the camera), kept
		// inside the view with room for the art around it: the arrow's tip.
		double screenX, screenY, depth;
		if (!projectToScreen(spot.pos, screenX, screenY, depth))
		{
			continue;
		}
		const int tipX = std::clamp((int)screenX, 6 * uiScale, width - 7 * uiScale),
			tipY = std::clamp((int)screenY, 14 * uiScale, height - 4 * uiScale);
		scaleOriginX = tipX;
		scaleOriginY = tipY;
		// An arrow pointing down, 7 wide at the top, outlined: first a black triangle one pixel
		// bigger all round (one more row on top, every row one wider on each side), then the
		// red triangle over it, then a black pixel under the red tip.
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int row = pass == 0 ? -1 : 0; row < 4; ++row)
			{
				const int halfWidth = 3 - std::max(row, 0) + (pass == 0 ? 1 : 0);
				for (int offset = -halfWidth; offset <= halfWidth; ++offset)
				{
					put(tipX + offset, tipY - 4 + row, pass == 0 ? _colors.tapeBlack : _colors.marker);
				}
			}
		}
		put(tipX, tipY, _colors.tapeBlack);
		// The enemy's number above it, in the tiny 3x5 digits (4 pixels apart), centred: first
		// a black outline around every lit dot, then the red dots.
		const std::string digits = std::to_string(spot.number);
		const int textWidth = (int)digits.size() * 4 - 1;
		const int textLeft = tipX - textWidth / 2, textTop = tipY - 11;
		for (int pass = 0; pass < 2; ++pass)
		{
			for (size_t digitIndex = 0; digitIndex < digits.size(); ++digitIndex)
			{
				const Uint8 *glyph = DigitFont[digits[digitIndex] - '0'];
				for (int glyphRow = 0; glyphRow < 5; ++glyphRow)
				{
					for (int glyphColumn = 0; glyphColumn < 3; ++glyphColumn)
					{
						// Bit 2 of a glyph row is its left column.
						if (!(glyph[glyphRow] & (4 >> glyphColumn))) continue;
						const int dotX = textLeft + (int)digitIndex * 4 + glyphColumn, dotY = textTop + glyphRow;
						if (pass == 0)
						{
							for (int outlineY = -1; outlineY <= 1; ++outlineY)
								for (int outlineX = -1; outlineX <= 1; ++outlineX)
									put(dotX + outlineX, dotY + outlineY, _colors.tapeBlack);
						}
						else
						{
							put(dotX, dotY, _colors.marker);
						}
					}
				}
			}
		}
	}
}

/**
 * A spray burst being marked (the game's "spray" auto shot, a player: Ctrl+Shift and clicks, as on
 * the map; see FirstPersonMode::updateSprayMarks), drawn over everything like the enemy markers
 * (as much bigger on big screens), in light green so it's seen far off too (a player: "light green
 * vertical lines with numbers and mini targets ... so the player really sees in 3d what he
 * marked"): at each point a pole standing up from the tile's floor, two thirds of a level high (and
 * at least SprayPoleMinPixels on the screen, for a tile far off), a mini target on it where the game
 * aims the rounds (the tile's middle, waist high) and the point's number beside it in the tiny digits;
 * a dotted line along the floors from point to point, and a grey one on to the tile under the
 * crosshair (where the next click marks). What's behind the camera is left out.
 */
void FirstPersonView::drawSprayMarks()
{
	if (_sprayPoints.empty())
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const int uiScale = std::max(1, _uiScale);
	// One dot of the marks' art, uiScale x uiScale, clipped to the view.
	auto put = [&](int x, int y, Uint8 color) {
		for (int offsetY = 0; offsetY < uiScale; ++offsetY)
			for (int offsetX = 0; offsetX < uiScale; ++offsetX)
				if (x + offsetX >= 0 && y + offsetY >= 0 && x + offsetX < width && y + offsetY < height)
					*getRaw(x + offsetX, y + offsetY) = color;
	};
	// A dot with a dark outline round it (both passes: first the outlines, then the colour).
	auto outlined = [&](int x, int y, Uint8 color, int pass) {
		if (pass == 0)
		{
			for (int offsetY = -1; offsetY <= 1; ++offsetY)
				for (int offsetX = -1; offsetX <= 1; ++offsetX)
					put(x + offsetX * uiScale, y + offsetY * uiScale, _colors.tapeBlack);
		}
		else
		{
			put(x, y, color);
		}
	};
	// A point on the screen, or false behind the camera.
	auto onScreen = [&](const Position &voxel, double up, int &x, int &y) {
		const double world[3] = { (double)voxel.x, (double)voxel.y, voxel.z + up };
		double screenX, screenY, depth;
		if (!projectToScreen(world, screenX, screenY, depth)) return false;
		x = (int)std::lround(screenX);
		y = (int)std::lround(screenY);
		return true;
	};
	// The dotted line from one point to the next along the floors, a dot every SprayDotSpacing voxels.
	auto dottedLine = [&](const Position &from, const Position &to, Uint8 color) {
		const double delta[3] = { (double)(to.x - from.x), (double)(to.y - from.y), (double)(to.z - from.z) };
		const double length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
		for (int pass = 0; pass < 2; ++pass)
		{
			for (double along = 0.0; along <= length; along += SprayDotSpacing)
			{
				const double share = length > 0.0 ? along / length : 0.0;
				const Position point((int)std::lround(from.x + delta[0] * share), (int)std::lround(from.y + delta[1] * share), (int)std::lround(from.z + delta[2] * share));
				int x, y;
				if (onScreen(point, 0.0, x, y)) outlined(x, y, color, pass);
			}
		}
	};
	for (size_t point = 0; point + 1 < _sprayPoints.size(); ++point)
	{
		dottedLine(_sprayPoints[point], _sprayPoints[point + 1], _colors.sprayMark);
	}
	dottedLine(_sprayPoints.back(), _sprayNext, _colors.sprayNext);

	// The marks: the pole, the mini target on it, the number over it.
	for (size_t point = 0; point < _sprayPoints.size(); ++point)
	{
		int footX, footY, topX, topY, aimX, aimY;
		const Position &floor = _sprayPoints[point];
		if (!onScreen(floor, 0.0, footX, footY) || !onScreen(floor, SprayPoleVoxels, topX, topY) || !onScreen(floor, SprayAimVoxels, aimX, aimY))
		{
			continue;
		}
		// A pole far off stands at least SprayPoleMinPixels high, the mini target the same share up it.
		const int minimum = SprayPoleMinPixels * uiScale;
		if (footY - topY < minimum)
		{
			topY = footY - minimum;
			aimY = footY - minimum * SprayAimVoxels / SprayPoleVoxels;
		}
		for (int pass = 0; pass < 2; ++pass)
		{
			// The pole, a dot every row (a lean tilts it: the steps between foot and top).
			const int rows = std::max(1, std::abs(footY - topY) / uiScale);
			for (int row = 0; row <= rows; ++row)
			{
				outlined(footX + (topX - footX) * row / rows, footY + (topY - footY) * row / rows, _colors.sprayMark, pass);
			}
			// The mini target: a ring SprayTargetRadius across and a dot in its middle.
			const int radius = SprayTargetRadius * uiScale;
			for (int step = 0; step < 24; ++step)
			{
				const double angle = step * Pi / 12.0;
				outlined(aimX + (int)std::lround(radius * std::cos(angle)), aimY + (int)std::lround(radius * std::sin(angle)), _colors.sprayMark, pass);
			}
			outlined(aimX, aimY, _colors.sprayMark, pass);
			// The number right of the mini target.
			const std::string digits = std::to_string(point + 1);
			const int textLeft = aimX + (SprayTargetRadius + 3) * uiScale, textTop = aimY - 2 * uiScale;
			for (size_t digitIndex = 0; digitIndex < digits.size(); ++digitIndex)
			{
				const Uint8 *glyph = DigitFont[digits[digitIndex] - '0'];
				for (int glyphRow = 0; glyphRow < 5; ++glyphRow)
				{
					for (int glyphColumn = 0; glyphColumn < 3; ++glyphColumn)
					{
						if (!(glyph[glyphRow] & (4 >> glyphColumn))) continue; // (bit 2 of a glyph row is its left column)
						outlined(textLeft + ((int)digitIndex * 4 + glyphColumn) * uiScale, textTop + glyphRow * uiScale, _colors.sprayMark, pass);
					}
				}
			}
		}
	}
}

/**
 * Icons over the downed units in sight (setting "Downed unit icons"): over each body lying
 * unconscious on a tile seen, one small outlined icon for each thing the matter with it, in the
 * order the game's own floor indicators take (where a mod has them): burning (a flame),
 * bleeding (a drop of blood: fatal wounds), shock (a bolt: health draining away), and always out
 * cold (Zz). Our own pixel art, drawn over everything like the enemy markers and as much bigger
 * on big screens, a little above the body on the screen, but only where the body itself is in
 * view (not behind a wall, nor in smoke too thick to see through) and within the view's reach.
 * With "Crosshair", only for the body the player points at (the one nearest the crosshair, or
 * the free cursor, within about a tile), just under the crosshair, where the shot info is read
 * too. A unit whose mod switches its indicators off gets none.
 */
void FirstPersonView::drawDownedIcons()
{
	if (Options::fpsDownedIcons <= 0)
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const int uiScale = std::max(1, _uiScale);
	const double focalLength = (width / 2.0) / _halfFovTangent;
	enum { Burning = 1, Bleeding = 2, Shock = 4, OutCold = 8 };

	// Each body in view: where its icons go (their bottom, in the middle), what's the matter with
	// it, and how far it is from where the player points (in about a tile's breadth on the
	// screen there).
	struct Downed { double screenX, screenY; int states; double fromPointer; };
	std::vector<Downed> downed;
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (tile->getInventory()->empty() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		int states = 0;
		for (const BattleItem *item : *tile->getInventory())
		{
			const BattleUnit *unit = item->getUnit();
			if (!unit || unit->getStatus() != STATUS_UNCONSCIOUS || !unit->indicatorsAreEnabled()) continue;
			states |= OutCold;
			if (unit->getFire() > 0) states |= Burning;
			if (unit->getFatalWounds() > 0) states |= Bleeding;
			if (unit->hasNegativeHealthRegen()) states |= Shock;
		}
		if (!states)
		{
			continue;
		}
		// The body on the floor in the middle of the tile, as drawFloorItems lays it: in view if
		// nothing nearer is drawn there (its own sprite is at its depth), and within reach, and
		// not on a floor above the eye (from under that floor it can't be seen, however near).
		// Seen from close by, its middle can be off the bottom of the view with the rest of it
		// on it: then a point a little higher up the body is looked at instead.
		const Position tilePosition = tile->getPosition();
		const double ground[3] = { tilePosition.x * 16.0 + 8.0, tilePosition.y * 16.0 + 8.0, tilePosition.z * 24.0 - tile->getTerrainLevel() };
		const double onFloor[3] = { ground[0], ground[1], standingHeight(ground) + 2.0 };
		if (_eye[2] < onFloor[2] - 2.0)
		{
			continue;
		}
		double screenX = 0.0, screenY = 0.0, depth = 0.0;
		bool inView = false;
		for (const double rise : { 0.0, 6.0 })
		{
			const double point[3] = { onFloor[0], onFloor[1], onFloor[2] + rise };
			if (!projectToScreen(point, screenX, screenY, depth) || depth > _maxDistance) break;
			const int pixelX = (int)screenX, pixelY = (int)screenY;
			if (pixelX < 0 || pixelY < 0 || pixelX >= width || pixelY >= height) continue;
			inView = _depthBuffer[pixelY * width + pixelX] >= depth - 8.0;
			break;
		}
		if (!inView)
		{
			continue;
		}
		// The icons a little above it on the screen: a body lies low (a third of a tile's breadth
		// there, at most 20 pixels of the art).
		const double tileOnScreen = std::max(1.0, 12.0 * focalLength / depth);
		downed.push_back({ screenX, screenY - std::min(tileOnScreen * 0.35, 20.0 * uiScale), states,
			std::hypot(screenX - _pointerX, screenY - _pointerY) / tileOnScreen });
	}
	// With "Crosshair", only the body pointed at, its icons just under the crosshair (which
	// reaches about 8 pixels of the art round where it points), below any name shown there.
	if (Options::fpsDownedIcons == 1)
	{
		auto pointedAt = std::min_element(downed.begin(), downed.end(), [](const Downed &a, const Downed &b) { return a.fromPointer < b.fromPointer; });
		if (pointedAt == downed.end() || pointedAt->fromPointer > 1.0)
		{
			return;
		}
		Downed only = *pointedAt;
		only.screenX = _pointerX;
		only.screenY = _pointerY + 17 * uiScale + _pointerTextBelow;
		downed.assign(1, only);
	}

	// The icons, at most 7 pixels high: '#' their colour, '+' a second one.
	struct Icon { int state; int width; const char *rows[7]; Uint8 color, second; };
	const Icon icons[] = {
		{ Burning, 5, { "..#..", ".##..", ".###.", "##+##", "#+++#", "#+++#", ".###." }, _colors.downedIcon[0], _colors.downedIcon[1] },
		{ Bleeding, 5, { "..#..", "..#..", ".###.", ".###.", "#####", "#####", ".###." }, _colors.downedIcon[2], _colors.downedIcon[2] },
		{ Shock, 5, { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..." }, _colors.downedIcon[3], _colors.downedIcon[3] },
		{ OutCold, 7, { "####...", "..#....", ".#.....", "####...", "....###", ".....#.", "....###" }, _colors.downedIcon[4], _colors.downedIcon[4] },
	};
	int originX = 0, originY = 0; // the art is drawn uiScale times bigger around this point
	// Puts one pixel of the art (unscaled coordinates on the view): a uiScale x uiScale block,
	// scaled out from the origin, clipped to the view.
	auto put = [&](int x, int y, Uint8 color) {
		const int pixelX = originX + (x - originX) * uiScale, pixelY = originY + (y - originY) * uiScale;
		for (int offsetY = 0; offsetY < uiScale; ++offsetY)
			for (int offsetX = 0; offsetX < uiScale; ++offsetX)
				if (pixelX + offsetX >= 0 && pixelY + offsetY >= 0 && pixelX + offsetX < width && pixelY + offsetY < height)
					*getRaw(pixelX + offsetX, pixelY + offsetY) = color;
	};
	for (const Downed &body : downed)
	{
		// Its icons in a row, two pixels apart, centred over it, their bottoms on the point.
		int rowWidth = -2;
		for (const Icon &icon : icons)
		{
			if (body.states & icon.state) rowWidth += icon.width + 2;
		}
		originX = (int)body.screenX;
		originY = (int)body.screenY;
		// First a black outline round every lit pixel, then the pixels.
		for (int pass = 0; pass < 2; ++pass)
		{
			int left = originX - rowWidth / 2;
			for (const Icon &icon : icons)
			{
				if (!(body.states & icon.state)) continue;
				for (int row = 0; row < 7; ++row)
				{
					for (int column = 0; column < icon.width; ++column)
					{
						const char dot = icon.rows[row][column];
						if (dot == '.') continue;
						const int x = left + column, y = originY - 7 + row;
						if (pass == 0)
						{
							for (int outlineY = -1; outlineY <= 1; ++outlineY)
								for (int outlineX = -1; outlineX <= 1; ++outlineX)
									put(x + outlineX, y + outlineY, _colors.tapeBlack);
						}
						else
						{
							put(x, y, dot == '+' ? icon.second : icon.color);
						}
					}
				}
				left += icon.width + 2;
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
