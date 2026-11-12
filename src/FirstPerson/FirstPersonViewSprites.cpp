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
#include "FirstPersonDamageNumbers.h"
#include "FirstPersonMode.h"
#include "../Engine/Font.h"
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
#include "FirstPersonSpriteHits.h"
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
 * A sprite of something lying on the floor (SpritePose::Lying: items, corpses, and a unit's art
 * below its feet) stands only above the anchor: its rows below the anchor, where the isometric art draws the front half of
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
	if (_gpuRecording)
	{
		gpuRecordSprite(draw); // (the card draws it: see FirstPersonViewGpuEffects.cpp)
		if (_gpuRecordOnly) return;
	}
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
			// Nothing of the card below the floor it stands on (see SpriteDraw::floorCut): the art's
			// rows under a unit's feet would hang through the ceiling of the room below.
			if (_eye[2] + ray[2] * distance / ZScale < draw.floorCut) return false;
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
	// glow behind it when it is more than half opaque. A unit's, a body's or an item's pixel
	// keeps its colours under futuristic night vision; anything else solid (smoke more than
	// half opaque) drawn over it takes that back (see _ownColours).
	const Uint8 ownColours = draw.unit || draw.body || draw.pile ? 1 : 0;
	auto plot = [&](int pixelX, int pixelY, Uint8 color, float depth, int pixelOpacity) {
		const int index = pixelY * width + pixelX;
		float &depthHere = _depthBuffer[index];
		if (!color || depth >= depthHere) return;
		// A pile's picture near the pointer, over the world behind it, solid or faint: noted on its own
		// (what's drawn after it doesn't take it back), so a faint pile stays faint with a unit behind.
		if (draw.pile)
		{
			const int pilePatchX = pixelX - _pointerX + PointerReach, pilePatchY = pixelY - _pointerY + PointerReach;
			if (pilePatchX >= 0 && pilePatchY >= 0 && pilePatchX <= 2 * PointerReach && pilePatchY <= 2 * PointerReach)
			{
				_pointerPiles[pilePatchY][pilePatchX] = draw.pile;
			}
		}
		if (pixelOpacity < SeeThroughLevels)
		{
			Uint8 &veilLeft = _unitVeil[index];
			pixelOpacity = std::min(pixelOpacity, (int)veilLeft);
			// Smoke builds up on a pixel to its cap and no further (settings "Smoke opacity" and
			// "Night smoke opacity", a maximum).
			const int smokeCap = seeThroughSprite ? seeThroughSprite->capLevels : 0;
			if (smokeCap > 0) pixelOpacity = std::min(pixelOpacity, smokeCap - (int)_smokeVeil[index]);
			if (pixelOpacity <= 0) return;
			if (veilLeft != 255) veilLeft -= pixelOpacity;
			if (smokeCap > 0) _smokeVeil[index] += (Uint8)pixelOpacity;
			Uint8 *pixel = getRaw(pixelX, pixelY);
			*pixel = seeThrough(pixelOpacity, shadeColor(color, shade), *pixel);
			if (pixelOpacity > SeeThroughLevels / 2)
			{
				_glowMask[index] = 0;
				_ownColours[index] = ownColours;
			}
			return;
		}
		if (opacity < 1.0f && dither(pixelX, pixelY) >= opacity) return;
		*getRaw(pixelX, pixelY) = shadeColor(color, shade);
		depthHere = depth;
		_glowMask[index] = 0;
		_unitVeil[index] = draw.unit ? UnitVeilMax : 255;
		_ownColours[index] = ownColours;
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
		return !seeThroughSprite ? SeeThroughLevels : seeThroughSprite->opacityMap ? (int)(seeThroughSprite->opacityMap[spriteY * spriteWidth + spriteX] * seeThroughOpacity + 0.5f)
			: (int)(SeeThroughLevels * seeThroughOpacity + 0.5f);
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
		// The row's screen pixels from `from` up to `to` that show its sprite columns from firstColumn up
		// to endColumn. A block of the view where everything drawn is nearer than the row (see
		// cullSmokeDraws) is passed over whole: every pixel of it would fail the depth test.
		const float *rowBlocks = seeThroughSprite && seeThroughSprite->depthBlocks ? seeThroughSprite->depthBlocks + (pixelY / SmokeCullBlock) * seeThroughSprite->depthBlockColumns : nullptr;
		auto drawColumns = [&](int from, int to, int firstColumn, int endColumn)
		{
			for (int pixelX = from; pixelX < to; ++pixelX)
			{
				if (rowBlocks && rowBlocks[pixelX / SmokeCullBlock] <= rowDepth)
				{
					pixelX = pixelX / SmokeCullBlock * SmokeCullBlock + SmokeCullBlock - 1;
					continue;
				}
				// (Its column: the floor of where the pixel's middle falls, which once it's past the first
				// column, never below 0, is the whole part: no floor to call for each pixel.)
				const double column = anchorX + (pixelX + 0.5 - rowAnchorX) / pixelsPerColumn;
				if (column < firstColumn) continue;
				const int spriteX = (int)column;
				if (spriteX >= endColumn) continue;
				plot(pixelX, pixelY, spritePixel(spriteX, spriteY), rowDepth, pixelOpacityAt(spriteX, spriteY));
			}
		};
		// A sprite with its rows' runs of coloured pixels noted (see-through smoke: the original's dots
		// leave most of a frame empty) only along those runs (a little more each side, each pixel's own
		// column still worked out as above, so the gaps, which draw nothing, are all that's left out).
		if (seeThroughSprite && seeThroughSprite->rowRuns)
		{
			for (int run = seeThroughSprite->rowRunStart[spriteY]; run < seeThroughSprite->rowRunStart[spriteY + 1]; ++run)
			{
				const int firstColumn = seeThroughSprite->rowRuns[run].first, endColumn = seeThroughSprite->rowRuns[run].second;
				drawColumns(std::max(left, (int)(rowAnchorX + (firstColumn - anchorX) * pixelsPerColumn) - 2),
					std::min(right, (int)(rowAnchorX + (endColumn - anchorX) * pixelsPerColumn) + 2), firstColumn, endColumn); // (two more pixels each side: no floor or ceiling to call)
			}
		}
		else
		{
			drawColumns(left, right, 0, spriteWidth);
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
	if (_gpuRecording)
	{
		// (Recorded for the card, in order: the funnels write each step down. Its check has the CPU draw them too, below.)
		const bool recordOnly = _gpuRecordOnly;
		_gpuRecordOnly = true;
		for (const DrawStep &step : _drawList)
		{
			switch (step.kind)
			{
			case DrawStep::Sprite:
				drawSprite(step.sprite);
				break;
			case DrawStep::GroundShade:
				drawGroundShade(step.centre, step.radius, step.steps, step.density, 0, 1 << 30, step.away, step.length);
				break;
			case DrawStep::Line:
				drawLine3D(step.from, step.to, step.color, step.thickness, step.opacity, 0.0f, 0, 1 << 30, step.glows);
				break;
			}
		}
		_gpuRecordOnly = recordOnly;
		if (!recordOnly && !_drawList.empty())
		{
			_gpuRecording = false;
			runOnAllThreads(&FirstPersonView::drawListBands);
			_gpuRecording = true;
		}
	}
	else if (!_drawList.empty())
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
				drawGroundShade(step.centre, step.radius, step.steps, step.density, bandTop, bandBottom, step.away, step.length);
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
 * Setting "Full sprite hitboxes": the height this view stands a unit on at a point (see
 * standingHeight), so that the lines of fire meet a unit's picture where the view draws it (see
 * FirstPersonSpriteHits.cpp): only once it has drawn the map (its snapshot of the tiles is there).
 * @param world The point on the ground, in voxels.
 * @param height Set to the height, in voxels.
 * @return False if the view hasn't drawn the map yet.
 */
/**
 * Setting "Full sprite hitboxes": the smooth (upscaled) picture of a unit this view drew last (up
 * close, setting "Sprite upscaling", see useSmoothVersion), if it drew this very frame smooth: the
 * same animation frame, seen from the same side, the same pixels as composed. The lines of the
 * soldier looked through meet that picture, as drawn (see FirstPersonSpriteHits.cpp).
 * @param unit The unit.
 * @param rotation The view rotation of its frame.
 * @param pixels The frame's solid pixels, as composed (SpriteBufferSize square).
 * @param factor Set to how many times bigger the smooth picture is.
 * @return The smooth picture, or null (not drawn smooth, or another frame).
 */
const Surface *FirstPersonView::smoothPicture(const BattleUnit *unit, int rotation, const std::vector<Uint8> &pixels, int &factor) const
{
	auto noted = _smoothPictures.find(unit);
	if (noted == _smoothPictures.end() || noted->second.animFrame != _save->getAnimFrame() || noted->second.rotation != rotation || noted->second.pixels != pixels)
	{
		return nullptr;
	}
	factor = noted->second.factor;
	return noted->second.smooth;
}

/**
 * Setting "Full sprite hitboxes": where this view drew a unit's feet in its last frame (see drawUnits:
 * a unit on the move between the tiles of its step, every unit eased between the game's small steps;
 * not the flinch of a hit), so that the lines of the soldier looked through meet its picture there.
 * @param unit The unit.
 * @param feet Set to the point, in voxels (on its floor, before the raise onto the floor's top).
 * @return False if the last frame didn't draw it.
 */
bool FirstPersonView::drawnFeet(const BattleUnit *unit, double feet[3]) const
{
	auto glide = _unitGlides.find(unit);
	if (glide == _unitGlides.end() || !_unitLastTick || glide->second.seen != _unitLastTick)
	{
		return false;
	}
	std::copy(glide->second.pos, glide->second.pos + 3, feet);
	return true;
}

bool FirstPersonView::standingHeightNow(const double world[3], double &height) const
{
	if (!_save || (int)_tileViews.size() != _save->getMapSizeXYZ())
	{
		return false;
	}
	height = standingHeight(world);
	return true;
}

/**
 * Setting "Full sprite hitboxes": the card a unit's picture stands on at a point, as drawSprite
 * draws it (the same reckoning): square to the view across the map, leaning back as the view looks
 * down (up to CardTiltMax) and forward as it looks up (CardTiltUpMax; a card steeply above the eye
 * leans towards it, up to CardTiltSteepMax). Gives one picture column to the right (SpritePixelX
 * of a voxel across, level) and one picture row up the card (a voxel up, upright), in voxels, both
 * times the scale, so that the lines of fire of the soldier looked through meet the very card drawn
 * (see FirstPersonSpriteHits.cpp).
 * @param world The anchor's point, in voxels.
 * @param scale The picture's scale (setting "Unit size").
 * @param column Set to one column to the right, in voxels.
 * @param row Set to one row up the card, in voxels.
 */
void FirstPersonView::cardAxes(const double world[3], double scale, double column[3], double row[3]) const
{
	const double level[2] = { std::cos(_yaw), std::sin(_yaw) }; // straight ahead, along the ground
	const double anchor[3] = { world[0] - _eye[0], world[1] - _eye[1], (world[2] - _eye[2]) * ZScale };
	const double elevation = std::atan2(anchor[2], std::sqrt(anchor[0] * anchor[0] + anchor[1] * anchor[1]));
	double tilt = std::clamp(_pitch, -CardTiltMax * Pi / 180.0, CardTiltUpMax * Pi / 180.0);
	if (elevation > CardTiltUpMax * Pi / 180.0)
	{
		tilt = std::max(tilt, std::min(elevation - CardTiltUpMax * Pi / 180.0, CardTiltSteepMax * Pi / 180.0));
	}
	// A column along the level right; a row up the card (cardUp in true proportions, a row
	// rowSize = scale * ZScale of it), its height back into voxels (divided by ZScale).
	const double levelRight[2] = { -level[1], level[0] };
	column[0] = levelRight[0] * SpritePixelX * scale;
	column[1] = levelRight[1] * SpritePixelX * scale;
	column[2] = 0.0;
	row[0] = -std::sin(tilt) * level[0] * scale * ZScale;
	row[1] = -std::sin(tilt) * level[1] * scale * ZScale;
	row[2] = std::cos(tilt) * scale;
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
 * The underside of the floor slab something standing at a point is on: the bottom of the point's
 * level, when that level's bottom layer of voxels is solid right under it (a floor, which is the
 * ceiling of the room below). A unit's sprite is cut off there (a player's report: units on a
 * floor above showed on the floor below them slightly): its art reaches a few rows below its
 * feet, the front half of the floor's diamond, and from below those hung through the ceiling. On
 * the ground (level 0) nothing lies below to show through, and over a hole, a stair's open side
 * or the air nothing is cut.
 * @param world The point on the floor, in voxels.
 * @return The height of the floor's underside, in voxels; NoFloorCut without a floor there.
 */
double FirstPersonView::floorUnderside(const double world[3]) const
{
	const int level = (int)std::floor(world[2] / Position::TileZ);
	if (level <= 0)
	{
		return NoFloorCut;
	}
	return solidVoxel((int)std::floor(world[0]), (int)std::floor(world[1]), level * Position::TileZ) ? level * (double)Position::TileZ : NoFloorCut;
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
 * Watches the units for hits and kills, once a frame, by their health, their stun and whether
 * they are out (unconscious or dead) compared with the last frame. A hit unit flinches back from
 * the camera and flashes (see drawUnits; a downed one's body flashes, see drawFloorItems); one in
 * sight sprays blood (or sparks, from machines), a downed one from its body on the floor; and an
 * enemy hit, downed or killed in sight marks the crosshair (the hitmarker). (A player, GitHub
 * issue #74: units already down showed nothing when hit; and a knocked-out unit mostly takes
 * stun, which counts as a hit now, for standing units too.)
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
			_watchedUnits.emplace(unit, UnitWatch{ unit->getHealth(), unit->getStunlevel(), isOut, unit->getStatus() == STATUS_DEAD, 0, { 0.0f, 0.0f } });
			continue;
		}
		// Did it lose health or take stun, go down (knocked out or killed) or die where it lay,
		// since the last frame?
		UnitWatch &watch = found->second;
		const bool dead = unit->getStatus() == STATUS_DEAD;
		const bool wasDown = watch.out;
		const bool wasHit = unit->getHealth() < watch.health || unit->getStunlevel() > watch.stun;
		const bool killed = (isOut && !watch.out) || (dead && !watch.dead);
		watch.health = unit->getHealth();
		watch.stun = unit->getStunlevel();
		watch.out = isOut;
		watch.dead = dead;
		if (!wasHit && !killed)
		{
			continue;
		}
		// Where it was hit: a standing unit's chest (the middle of its tiles, 60% of its height
		// up); one already down, its body, a little above the floor it lies on (none while it's
		// carried); and the level direction from the camera to it: dx, dy from the eye,
		// flatDistance their length. Seen: the unit in sight; a body, where the view draws it.
		float chest[3];
		bool seen;
		if (wasDown)
		{
			const BattleItem *body = nullptr;
			for (const BattleItem *item : *_save->getItems())
			{
				if (item->getUnit() == unit && item->getTile())
				{
					body = item;
					break;
				}
			}
			if (!body)
			{
				continue;
			}
			const Tile *bodyTile = body->getTile();
			const Position at = bodyTile->getPosition();
			chest[0] = at.x * 16.0f + 8.0f;
			chest[1] = at.y * 16.0f + 8.0f;
			chest[2] = at.z * 24.0f - bodyTile->getTerrainLevel() + BodySprayHeight;
			// (seen as the view draws it, on a tile seen before, as the map shows what lies there:
			// the game's sight may leave out a body just below a ledge the soldier stands on)
			seen = _tileViews[_save->getTileIndex(at)].discovered || bodyTile->getVisible() || _save->getDebugMode();
		}
		else
		{
			const Position tilePosition = unit->getPosition();
			const int unitSize = unit->getArmor()->getSize(); // tiles across: 1, or 2 for big units
			chest[0] = tilePosition.x * 16.0f + 8.0f * unitSize;
			chest[1] = tilePosition.y * 16.0f + 8.0f * unitSize;
			chest[2] = tilePosition.z * 24.0f + unit->getFloatHeight() + unit->getHeight() * 0.6f;
			seen = unit->getVisible() || _save->getDebugMode();
		}
		const float dx = chest[0] - (float)_eye[0], dy = chest[1] - (float)_eye[1];
		const float flatDistance = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
		// A hit starts the flinch, pushing it away from the camera (straight above or below it, no
		// way across the map is away from the camera: none, see FirstPerson::unitViewRotation).
		if (wasHit)
		{
			const bool straightAbove = flatDistance < 1.0f;
			watch.hitTick = now;
			watch.push[0] = straightAbove ? 0.0f : dx / flatDistance;
			watch.push[1] = straightAbove ? 0.0f : dy / flatDistance;
		}
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
		// An enemy hit, downed or killed in sight: the hitmarker on the crosshair.
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
 * soft shadow. Also notes where the numbered enemy markers go this frame (drawMarkers), and
 * outlines their hitboxes (setting "Hitboxes"; with "Full sprite hitboxes", the pictures the lines
 * from the eye test: see noteTestedPictures, drawUnitOutlines). The unit being looked through is not drawn. Their pixels are noted as a unit's, so the
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
	// on the same spot), and their hitboxes (setting): always, fading in and out once a second, or
	// only the unit pointed at (Aimed at, see setAimedUnit), steady.
	const double unitScale = std::clamp(Options::fpsUnitSize, 80, 100) / 100.0;
	const bool hitboxAimedOnly = Options::fpsHitboxes == 3;
	const float hitboxOpacity = Options::fpsHitboxes == 2 || hitboxAimedOnly ? 1.0f
		: Options::fpsHitboxes == 1 ? 0.5f + 0.5f * (float)std::cos((now % 1000) / 1000.0 * 2.0 * Pi) : 0.0f;
	// With "Full sprite hitboxes" the hitbox is the picture: the same setting outlines that instead,
	// a line round it (the owner: "the sprites should be then surrounded by red line to show where you
	// can hit them"): round what the lines from the eye test, from the test's own cards (a player:
	// "a visualization of the actual hitbox would help"), noted once the units are drawn and drawn
	// over them (see noteTestedPictures, drawUnitOutlines).
	_outlining = hitboxOpacity > 0.05f && Options::fpsSpriteHitboxes;
	_outlineColors.clear();
	_outlinedUnits.clear();
	_smoothPictures.clear(); // (noted afresh for the units drawn smooth this frame; a smooth picture unused for a frame may go)
	_outlineOpacity = hitboxOpacity;
	if (_outlining)
	{
		const size_t pixels = (size_t)getWidth() * getHeight();
		if (_outlineOwner.size() != pixels) _outlineOwner.assign(pixels, 0);
		if (_outlineDepth.size() != pixels) _outlineDepth.assign(pixels, 0.0f);
		_outlineRowLeft.assign(getHeight(), getWidth());
		_outlineRowRight.assign(getHeight(), -1);
	}

	// Notes the markers over a unit (see drawMarkers), at a point over it: one for each of its buttons that the mode
	// marks (an enemy has one; one of our own can have two, wounded and about to pass out), stacked as the game stacks
	// the buttons, the first at the bottom; and those of a body it carries (see EnemyMarker).
	auto noteMarkers = [&](const BattleUnit *unit, const double over[3]) {
		const size_t first = _markerSpots.size();
		for (const auto &marker : _markers)
		{
			if ((marker.carrier ? marker.carrier : marker.unit) == unit)
			{
				_markerSpots.push_back({ { over[0], over[1], over[2] }, marker.number, marker.kind, (int)(_markerSpots.size() - first), 0 });
			}
		}
		for (size_t spot = first; spot < _markerSpots.size(); ++spot)
		{
			_markerSpots[spot].stackSize = (int)(_markerSpots.size() - first);
		}
	};

	// The units are recorded first (their shadows, sprites and hitboxes, in order), then drawn
	// on all the view's threads at once (see replayDrawList). Each unit's frame is composed in
	// the sprite buffer, then kept in a frame of its own until then.
	int framesUsed = 0;
	for (BattleUnit *unit : *_save->getUnits())
	{
		// Only units still in the fight (not dead or unconscious), not the one looked through,
		// and in sight (any unit in debug mode). And one down a moment ago with "Quick deaths",
		// still falling (on its own, see FirstPerson::quickFallPhase): drawn in that fall. And one
		// gone down in the air, coming down to the ground (FirstPerson::airFallHeight): drawn
		// falling, lying once its own fall frames are over. And one a chain of explosions settled
		// at once took down (setting "Instant chain explosions"): standing as it was until the
		// chain's ripple has passed, the dead falling after it (FirstPerson::chainHeldStanding).
		const int chainFacing = unit->isOut() ? FirstPerson::chainHeldStanding(_save, unit) : -1;
		const bool chainHeld = chainFacing >= 0;
		const bool quickFall = !chainHeld && unit->isOut() && FirstPerson::quickFallPhase(unit) >= 0;
		Position airFallTile;
		double airFallFeetZ = 0.0;
		const bool airFall = !chainHeld && FirstPerson::airFallHeight(unit, airFallTile, airFallFeetZ);
		const bool fallingDown = quickFall || (airFall && unit->isOut());
		const bool lying = unit->isOut() && !fallingDown && !chainHeld;
		// One of our own lying out cold with markers (setting "Wounded markers": the game's blue or purple buttons for
		// it), not drawn here: its markers over the body, 5 voxels over its middle where it lies on its tile (as
		// drawDownedIcons finds it); a body carried has no tile (its markers go over the unit carrying it).
		if (lying && !_markers.empty() && unit->getStatus() == STATUS_UNCONSCIOUS && unit != _cameraUnit)
		{
			const Position bodyTile = unit->getPosition();
			if (const Tile *tile = _save->getTile(bodyTile))
			{
				const int bodySize = unit->getArmor()->getSize();
				const double ground[3] = { bodyTile.x * 16.0 + 8.0 * bodySize, bodyTile.y * 16.0 + 8.0 * bodySize, bodyTile.z * 24.0 - tile->getTerrainLevel() };
				const double overBody[3] = { ground[0], ground[1], standingHeight(ground) + 2.0 + 5.0 };
				noteMarkers(unit, overBody);
			}
		}
		if (lying || unit == _cameraUnit || (!debug && !unit->getVisible() && !chainHeld))
		{
			continue;
		}
		// (a unit held standing is up, as it was, for all that's drawn of it here)
		const FirstPerson::ChainStandingPose chainPose(chainHeld ? unit : nullptr, chainFacing);
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
		if (airFall)
		{
			// Coming down from the air: over the tile it hovered over, as low as it has fallen.
			place(airFallTile, feet);
			feet[2] = airFallFeetZ;
		}
		// The game moves units in small steps a few times a second: glide between them
		// (but snap on anything big: a unit seen anew, teleported, falling a floor; and follow a
		// fall from the air as it is).
		Glide &glide = _unitGlides[unit];
		const double jump = std::abs(glide.pos[0] - feet[0]) + std::abs(glide.pos[1] - feet[1]) + std::abs(glide.pos[2] - feet[2]);
		if (now - glide.seen > 250 || jump > 24.0 || airFall)
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
		const double placedFeet[3] = { feet[0], feet[1], feet[2] }; // (before a hit's flinch: the side it's seen from, below)
		// A hit unit flinches back from the camera for a moment (HitFlinchTime, up to 3 voxels), and
		// flashes (setting "Hit flash", and "Muzzle flashes", which turns every flash off).
		float flash = 0.0f;
		auto watched = _watchedUnits.find(unit);
		if (watched != _watchedUnits.end() && watched->second.hitTick && now - watched->second.hitTick < HitFlinchTime)
		{
			const float flinch = 1.0f - (now - watched->second.hitTick) / (float)HitFlinchTime; // 1 at the hit, down to 0
			feet[0] += watched->second.push[0] * 3.0 * flinch;
			feet[1] += watched->second.push[1] * 3.0 * flinch;
			flash = Options::fpsMuzzleFlash && Options::fpsHitFlash ? flinch : 0.0f;
		}

		// A marked unit's markers go 5 voxels above its head (not over one falling: it's down).
		if (!fallingDown)
		{
			const double overHead[3] = { feet[0], feet[1], feet[2] + unit->getFloatHeight() + unit->getHeight() + 5.0 };
			noteMarkers(unit, overHead);
		}
		// An enemy the squad sees but the soldier looked through doesn't, with smoke on the line to
		// it: not drawn, only its marker, as for one behind a wall (a player's report, GitHub issue
		// #98: "if a unit is visible, let me see it, if a unit is invisible, don't let me see it").
		// Enemies the soldier sees stay clear of the smoke (see _unitVeil).
		if (_cameraUnit && !debug && !fallingDown && unit->getFaction() == FACTION_HOSTILE && !_cameraUnit->hasVisibleUnit(unit))
		{
			const double middle[3] = { feet[0], feet[1], feet[2] + unit->getFloatHeight() + unit->getHeight() * 0.5 };
			if (smokeSightUsedTo(middle) > 0.0f)
			{
				continue;
			}
		}

		// The isometric sprites show units as seen from the south-east (direction 3).
		// Rotate the unit's facing so the side turned towards the camera is shown: the
		// direction from the unit to the camera, in the game's eight directions, from the eye
		// as placed to where the unit stands (neither a shake nor a hit's flinch turns it);
		// straight above or below it, from the south-east, as the map shows it (see
		// FirstPerson::unitViewRotation: the lines of fire test the same picture).
		const int viewRotation = FirstPerson::unitViewRotation(_steadyEye, placedFeet);
		unitSprite.setViewRotation(viewRotation);

		// The light on it, at its chest: the game's light, lamps, flashlight and flares, and
		// the flash of a hit; in the map's first debug vision mode no darker than
		// NightVisionShade, as the map draws units then (see debugVision).
		_spriteBuffer->clear();
		const float chest[3] = { (float)feet[0], (float)feet[1], (float)feet[2] + 12.0f };
		int shade = (int)std::lround(std::clamp(lightAt(chest[0], chest[1], chest[2]) - pointLight(chest, nullptr, false)
			- flareLight(chest, nullptr, false) * 8.0f - flash * HitFlashShade, 0.0f, 15.0f));
		if (debugVision() == 1) shade = std::min(shade, NightVisionShade);
		if (Options::fpsShadows)
		{
			recordUnitShadow(feet, 5.5 * unitSize); // flying units too: the shadow on the ground shows where they hover
			recordFlareShadow(unit, feet, 5.5 * unitSize);
		}
		// Compose the unit's frame in the sprite buffer and keep it in a frame of its own, then
		// record it standing in the world, on the floor's surface (the hitbox and the markers
		// keep the game's own heights). (Composed unlit and shaded when placed: an upscaled frame
		// then serves in any light.)
		const GraphSubset mask(SpriteBufferSize, SpriteBufferSize);
		{
			// (a quick fall's frame; a body coming down from the air after its own fall frames, the last)
			const int fallFrame = quickFall ? FirstPerson::quickFallPhase(unit) : fallingDown ? std::max(0, unit->getArmor()->getDeathFrames() - 1) : -1;
			const FirstPerson::QuickFallPose fallPose(fallingDown ? unit : nullptr, fallFrame);
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
		}
		DrawStep step;
		step.kind = DrawStep::Sprite;
		step.sprite.sprite = keepComposedFrame(framesUsed);
		step.sprite.anchorX = 32;
		step.sprite.anchorY = unitSize == 1 ? 40 : 48; // the centre of the floor diamond; a big unit's, the corner its four tiles share
		step.sprite.world[0] = feet[0];
		step.sprite.world[1] = feet[1];
		step.sprite.world[2] = standingHeight(feet);
		step.sprite.floorCut = floorUnderside(feet); // (its art below its feet stays out of the room below)
		step.sprite.shade = shade;
		step.sprite.scale = unitScale;
		// The art below its feet (the front half of the floor's diamond: a snake's tail, a long
		// body in some facings) lies flat on the floor in front of them, as an item's does, instead
		// of sinking into it (a player's report, GitHub issue #31: X-Com Files' Asmodean Cobra "in
		// the ground" in some rotations; for most units only a few pixels of the feet). Not for a
		// unit in the air: its floor would be seen from below, where lying rows aren't drawn.
		if (!unit->haveNoFloorBelow() && unit->getStatus() != STATUS_FLYING)
		{
			step.sprite.pose = SpritePose::Lying;
		}
		step.sprite.unit = unit;
		useSmoothVersion(step.sprite);
		// With full sprite hitboxes, a picture drawn smooth (upscaled, up close) is the one the lines of the
		// soldier looked through meet: noted with the frame it is (see smoothPicture).
		if (Options::fpsSpriteHitboxes)
		{
			if (step.sprite.scale < unitScale)
			{
				SmoothPicture &noted = _smoothPictures[unit];
				noted.animFrame = _save->getAnimFrame();
				noted.rotation = viewRotation;
				noted.pixels.resize((size_t)SpriteBufferSize * SpriteBufferSize);
				const SDL_Surface *composed = _spriteBuffer->getSurface(); // (still the frame composed)
				for (int y = 0; y < SpriteBufferSize; ++y)
				{
					for (int x = 0; x < SpriteBufferSize; ++x) noted.pixels[(size_t)y * SpriteBufferSize + x] = ((const Uint8 *)composed->pixels)[y * composed->pitch + x] != 0;
				}
				noted.smooth = step.sprite.sprite;
				noted.factor = spriteUpscaleFactor();
			}
			else
			{
				_smoothPictures.erase(unit);
			}
		}
		// With full sprite hitboxes, what the lines test outlined where the box would be drawn (see
		// noteTestedPictures); not for one so close that it fades out, dithered (drawSprite: nearer
		// than 10 voxels), where the holes would be outlined.
		double screenX, screenY, screenDepth;
		if (_outlining && (!hitboxAimedOnly || unit == _aimedUnit) && !fallingDown && _outlineColors.size() < 0xFFFF
			&& projectToScreen(step.sprite.world, screenX, screenY, screenDepth) && screenDepth >= 10.0)
		{
			_outlineColors.push_back(_colors.hitbox[unit->getFaction() == FACTION_PLAYER ? 0 : unit->getFaction() == FACTION_HOSTILE ? 1 : 2]);
			OutlinedUnit outlined;
			outlined.unit = unit;
			outlined.outline = (Uint16)_outlineColors.size();
			_outlinedUnits.push_back(outlined);
		}
		_drawList.push_back(step);
		if (hitboxOpacity > 0.05f && (!hitboxAimedOnly || unit == _aimedUnit) && !Options::fpsSpriteHitboxes && !fallingDown) // (with full sprite hitboxes the picture is the hitbox, outlined above; one falling is down)
		{
			recordHitbox(unit, feet, hitboxOpacity);
		}
		// Which way it faces, with the key for it held (see drawFacing): round its footprint (inside
		// its tiles, so neighbours' rings don't cross), at the bottom of its hitbox (under a flyer),
		// in the colour of its side.
		if (_showFacing && !fallingDown)
		{
			FacingSpot spot = { { feet[0], feet[1], feet[2] + unit->getFloatHeight() + 1.0 }, 8.0 * unitSize - 1.5, unit->getDirection(),
				_colors.hitbox[unit->getFaction() == FACTION_PLAYER ? 0 : unit->getFaction() == FACTION_HOSTILE ? 1 : 2] };
			_facingSpots.push_back(spot);
		}
	}
	// Then draw them all, and with full sprite hitboxes the outlines of what the lines test over them.
	replayDrawList();
	if (_outlining)
	{
		noteTestedPictures();
		drawUnitOutlines();
	}
}

/**
 * Setting "Hitboxes" with "Full sprite hitboxes": notes on the screen where each outlined unit's
 * picture is hit by the lines from the eye (the test's own card, see FirstPerson::viewCard: in the
 * view showing the battle, the very card drawn, its feet on the floor's top; the art below the feet
 * lying on the floor is never hit, nor what the picture reaches below a floor), so that the outline
 * goes round that (see drawUnitOutlines): which unit's on each pixel, how far ahead, and each row's
 * first and last such pixel. Only where it shows: nothing nearer drawn there (a wall, a unit in
 * front; its own drawn picture is as near). The cards are worked out here, on the game's thread;
 * the pixels on all the view's threads (see testedPictureBands).
 */
void FirstPersonView::noteTestedPictures()
{
	const int width = getWidth(), height = getHeight();
	// (in the view showing the battle the card tested is the one drawn: as near as its picture; in
	// another camera's it stands upright, a little off the picture drawn)
	_testedPictureSlack = this == FirstPersonMode::shootingView(_save->getBattleState()) ? TestedPictureDepthSlack : TestedPictureDepthSlackElsewhere;
	std::vector<OutlinedUnit> cards;
	for (OutlinedUnit &outlined : _outlinedUnits)
	{
		if (!FirstPerson::viewCard(_save, outlined.unit, this, outlined.card))
		{
			continue;
		}
		// The picture's solid pixels' bounds (the rows below the feet only when they count).
		const FirstPerson::SpriteCard &card = outlined.card;
		int firstColumn = SpriteBufferSize, lastColumn = -1, firstRow = SpriteBufferSize, lastRow = -1;
		for (int row = 0; row < (card.rowsBelow ? SpriteBufferSize : card.anchorY); ++row)
		{
			for (int column = 0; column < SpriteBufferSize; ++column)
			{
				if (!(*card.picture)[(size_t)row * SpriteBufferSize + column]) continue;
				firstColumn = std::min(firstColumn, column); lastColumn = std::max(lastColumn, column);
				firstRow = std::min(firstRow, row); lastRow = std::max(lastRow, row);
			}
		}
		if (lastColumn < 0)
		{
			continue;
		}
		// The screen box round those bounds' corners on the card (all of the view if one is behind the eye).
		outlined.left = width; outlined.top = height; outlined.right = -1; outlined.bottom = -1;
		for (const int column : { firstColumn, lastColumn + 1 })
		{
			for (const int row : { firstRow, lastRow + 1 })
			{
				double corner[3], screenX, screenY, depth;
				for (int axis = 0; axis < 3; ++axis) corner[axis] = card.anchor[axis] + (column - card.anchorX) * card.column[axis] + (card.anchorY - row) * card.row[axis];
				if (!projectToScreen(corner, screenX, screenY, depth))
				{
					outlined.left = 0; outlined.top = 0; outlined.right = width - 1; outlined.bottom = height - 1;
					continue;
				}
				outlined.left = std::min(outlined.left, (int)std::floor(screenX) - 2);
				outlined.right = std::max(outlined.right, (int)std::ceil(screenX) + 2);
				outlined.top = std::min(outlined.top, (int)std::floor(screenY) - 2);
				outlined.bottom = std::max(outlined.bottom, (int)std::ceil(screenY) + 2);
			}
		}
		outlined.left = std::max(0, outlined.left); outlined.top = std::max(0, outlined.top);
		outlined.right = std::min(width - 1, outlined.right); outlined.bottom = std::min(height - 1, outlined.bottom);
		if (outlined.left <= outlined.right && outlined.top <= outlined.bottom)
		{
			cards.push_back(outlined);
		}
	}
	_outlinedUnits.swap(cards);
	if (!_outlinedUnits.empty())
	{
		runOnAllThreads(&FirstPersonView::testedPictureBands);
	}
}

/**
 * The tested pictures (see noteTestedPictures) over bands of the view's rows, taken one at a time
 * from the shared counter until none are left: a job for all the view's threads. Each pixel's ray
 * from the eye meets each card where FirstPerson::cardRay finds it, as a line of fire would; a
 * solid pixel there that nothing nearer covers is noted (the nearest unit's, where two overlap).
 */
void FirstPersonView::testedPictureBands()
{
	const int width = getWidth(), height = getHeight();
	const int bandCount = (height + BandRows - 1) / BandRows;
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		const int bandBottom = std::min(height, band * BandRows + BandRows);
		for (int pixelY = band * BandRows; pixelY < bandBottom; ++pixelY)
		{
			for (const OutlinedUnit &outlined : _outlinedUnits)
			{
				if (pixelY < outlined.top || pixelY > outlined.bottom) continue;
				for (int pixelX = outlined.left; pixelX <= outlined.right; ++pixelX)
				{
					double ray[3], along;
					rayDirectionThrough(pixelX, pixelY, ray);
					if (!FirstPerson::cardRay(outlined.card, _eye, ray, along) || along <= 0.0) continue;
					// How far ahead it is, as the depth buffer measures (true proportions, along the view).
					const double ahead = ray[0] * along * _forward[0] + ray[1] * along * _forward[1] + ray[2] * along * ZScale * _forward[2];
					const int index = pixelY * width + pixelX;
					if (ahead < 1.0 || ahead > _depthBuffer[index] + _testedPictureSlack) continue;
					if (_outlineOwner[index] && _outlineDepth[index] <= ahead) continue;
					_outlineOwner[index] = outlined.outline;
					_outlineDepth[index] = (float)ahead;
					_outlineRowLeft[pixelY] = std::min(_outlineRowLeft[pixelY], pixelX);
					_outlineRowRight[pixelY] = std::max(_outlineRowRight[pixelY], pixelX);
				}
			}
		}
	}
}

/**
 * Setting "Hitboxes" with "Full sprite hitboxes", where the hitbox is the unit's picture: outlines
 * what the lines from the eye test of each picture (noted by noteTestedPictures), a line a pixel wide just
 * outside its edge in the colour of its side, as the boxes are drawn. A pixel is outlined when it
 * isn't that unit's, borders one of its pixels (the eight round it) and what shows there is behind
 * the unit (farther than that pixel of it): the picture's silhouette against what's behind it, so
 * a wall or a unit in front of part of it is never outlined (nor the floor in front of its feet).
 * A pixel bordering more than one outlined picture it's behind takes the nearest's colour. Like the
 * boxes' lines it fades in and out with Blink (dithered), glows with "Glowing outlines", and
 * whatever is drawn after the units (fire, smoke, shots) covers it. Then the notes are cleared.
 * Only the rows the outlined pictures cover are looked at, a pixel either side of each row's.
 */
void FirstPersonView::drawUnitOutlines()
{
	if (!_outlineColors.empty())
	{
		runOnAllThreads(&FirstPersonView::outlineBands);
	}
	// Leave the notes all 0 for the next frame: only the rows' spans were written.
	const int width = getWidth();
	for (int row = 0; row < (int)_outlineRowLeft.size(); ++row)
	{
		if (_outlineRowLeft[row] <= _outlineRowRight[row])
		{
			std::fill(_outlineOwner.begin() + row * width + _outlineRowLeft[row], _outlineOwner.begin() + row * width + _outlineRowRight[row] + 1, (Uint16)0);
		}
	}
	_outlining = false;
}

/**
 * The outlines round the units' pictures (see drawUnitOutlines) over bands of the view's rows,
 * taken one at a time from the shared counter until none are left: a job for all the view's
 * threads (see runOnAllThreads). Each pixel is worked out from the notes and the depth buffer
 * alone, which nothing here changes, so the bands don't depend on each other.
 */
void FirstPersonView::outlineBands()
{
	const int width = getWidth(), height = getHeight();
	const int bandCount = (height + BandRows - 1) / BandRows;
	const Uint8 glows = Options::fpsGlowingOutlines ? 1 : 0; // (setting "Glowing outlines")
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		const int bandBottom = std::min(height, band * BandRows + BandRows);
		for (int pixelY = band * BandRows; pixelY < bandBottom; ++pixelY)
		{
			// The pixels that may border an outlined picture: a pixel either side of the span of this
			// row's, the row above's and the row below's.
			int left = width, right = -1;
			for (int row = std::max(0, pixelY - 1); row <= std::min(height - 1, pixelY + 1); ++row)
			{
				left = std::min(left, _outlineRowLeft[row] - 1);
				right = std::max(right, _outlineRowRight[row] + 1);
			}
			left = std::max(0, left);
			right = std::min(width - 1, right);
			for (int pixelX = left; pixelX <= right; ++pixelX)
			{
				const int index = pixelY * width + pixelX;
				const Uint16 here = _outlineOwner[index];
				// The nearest outlined picture beside it that it's behind, if any (by how far ahead the
				// tested picture is there).
				Uint16 outline = 0;
				float nearest = _depthBuffer[index];
				for (int offsetY = -1; offsetY <= 1; ++offsetY)
				{
					const int y = pixelY + offsetY;
					if (y < 0 || y >= height) continue;
					for (int offsetX = -1; offsetX <= 1; ++offsetX)
					{
						const int x = pixelX + offsetX;
						if (x < 0 || x >= width) continue;
						const Uint16 owner = _outlineOwner[y * width + x];
						if (owner && owner != here && _outlineDepth[y * width + x] < nearest)
						{
							outline = owner;
							nearest = _outlineDepth[y * width + x];
						}
					}
				}
				if (!outline || (_outlineOpacity < 1.0f && dither(pixelX, pixelY) >= _outlineOpacity)) continue;
				*getRaw(pixelX, pixelY) = _outlineColors[outline - 1];
				_glowMask[index] = glows;
			}
		}
	}
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
 * Only what's drawn counts, so only units in sight, standing, and not hidden behind anything
 * (and not one down a moment ago, still drawn falling with "Quick deaths": it's down).
 * The same for a body lying on the floor (an unconscious unit's, see getBodyAtPointer), when
 * its picture is nearer the pointer than any standing unit's (a medi-kit's patient); and for the
 * pile whose picture is there over the world behind it, whatever is drawn in front (it turns
 * faint the next frame if the pointer aims past it, see drawFloorItems).
 */
void FirstPersonView::findUnitAtPointer()
{
	_unitAtPointer = nullptr;
	_bodyAtPointer = nullptr;
	_pileAtPointer = nullptr;
	int nearestUnit = 1 << 30, nearestBody = 1 << 30, nearestPile = 1 << 30;
	for (int patchY = 0; patchY <= 2 * PointerReach; ++patchY)
	{
		for (int patchX = 0; patchX <= 2 * PointerReach; ++patchX)
		{
			const int distanceSq = (patchX - PointerReach) * (patchX - PointerReach) + (patchY - PointerReach) * (patchY - PointerReach);
			if (_pointerUnits[patchY][patchX] && !_pointerUnits[patchY][patchX]->isOut() && distanceSq < nearestUnit)
			{
				nearestUnit = distanceSq;
				_unitAtPointer = _pointerUnits[patchY][patchX];
			}
			if (_pointerBodies[patchY][patchX] && distanceSq < nearestBody)
			{
				nearestBody = distanceSq;
				_bodyAtPointer = _pointerBodies[patchY][patchX];
			}
			if (_pointerPiles[patchY][patchX] && distanceSq < nearestPile)
			{
				nearestPile = distanceSq;
				_pileAtPointer = _pointerPiles[patchY][patchX];
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
 * recorded, to be drawn with the units (see drawUnits, replayDrawList). It lies on the floor's
 * surface, where the unit stands in the view (see standingHeight): at the game's foot of the
 * floor, inside the slab, the floor seen past a few tiles off missed it (GitHub issue #73).
 * @param feet The point on the ground under the unit's middle, in voxels.
 * @param radius The patch's radius, in voxels.
 */
void FirstPersonView::recordUnitShadow(const double feet[3], double radius)
{
	DrawStep step;
	step.kind = DrawStep::GroundShade;
	std::copy(feet, feet + 3, step.centre);
	step.centre[2] = standingHeight(feet);
	step.radius = radius;
	step.steps = 2;
	step.density = 1.8f;
	step.length = 0.0;
	_drawList.push_back(step);
}

/**
 * A unit's shadow from the tactical flare lighting it most (a player, GitHub issue #73: "let tac
 * flares cast unit shadows"; the owner: a stretched soft shadow): a soft dark patch on the ground
 * from its feet away from the flare, as long as the unit's height thrown from the flare's (up to
 * FlareShadowMaxLength: a flare on the ground throws it that far), widening towards its end and
 * fading out there, as dark as the flare is bright at the unit. None from a flare the unit can't
 * see (a ray from its chest), nor from one fainter than FlareShadowMinPower or right under it.
 * Recorded, to be drawn with the units (see drawGroundShade).
 * @param unit The unit.
 * @param feet The point on the ground under the unit's middle, in voxels.
 * @param radius Its soft shadow's radius, in voxels (the patch's width at the feet).
 */
void FirstPersonView::recordFlareShadow(const BattleUnit *unit, const double feet[3], double radius)
{
	// The flare that lights the unit most (as they light a unit: no adding up, see flareLight).
	const float chest[3] = { (float)feet[0], (float)feet[1], (float)feet[2] + 12.0f };
	const FlareLight *brightest = nullptr;
	float brightestPower = 0.0f;
	for (const FlareLight &flare : _flareLights)
	{
		const float power = flarePower(flare, std::sqrt((flare.pos[0] - chest[0]) * (flare.pos[0] - chest[0])
			+ (flare.pos[1] - chest[1]) * (flare.pos[1] - chest[1]) + (flare.pos[2] - chest[2]) * (flare.pos[2] - chest[2])));
		if (power > brightestPower)
		{
			brightest = &flare;
			brightestPower = power;
		}
	}
	if (!brightest || brightestPower < FlareShadowMinPower || shadowed(chest, brightest->pos))
	{
		return;
	}
	// Away from the flare along the ground, and how far: the unit's top (at the pictures' size)
	// thrown from the flare's height onto the ground, or the longest for a flare no higher.
	const double awayX = feet[0] - brightest->pos[0], awayY = feet[1] - brightest->pos[1], across = std::hypot(awayX, awayY);
	if (across < 1.0)
	{
		return;
	}
	const double height = (unit->getFloatHeight() + unit->getHeight()) * Options::fpsUnitSize / 100.0;
	const double flareAbove = brightest->pos[2] - feet[2];
	const double length = flareAbove > height + 1.0 ? std::min(FlareShadowMaxLength, across * height / (flareAbove - height)) : FlareShadowMaxLength;
	DrawStep step;
	step.kind = DrawStep::GroundShade;
	std::copy(feet, feet + 3, step.centre);
	step.centre[2] = standingHeight(feet); // (on the floor's surface, see recordUnitShadow)
	step.radius = radius;
	step.steps = std::clamp((int)std::lround(brightestPower * FlareShadowShades), 1, FlareShadowMaxSteps);
	step.density = 1.4f;
	step.away[0] = awayX / across;
	step.away[1] = awayY / across;
	step.length = std::max(length, radius);
	_drawList.push_back(step);
}

/**
 * Darkens the ground in a soft disc (unit shadows, scorched earth), or in a patch stretched away
 * from a light (a unit's shadow from a flare, see recordFlareShadow): pixels whose floor lies
 * within it lose up to `steps` shades, thinning out (dithered) towards the edge (and a stretched
 * patch towards its end too, widening there). Only pixels showing the floor right there are
 * darkened, not a wall or a unit in front of it.
 * @param centre The disc's centre on the ground, in voxels (a stretched patch's start).
 * @param radius The disc's radius, in voxels (a stretched patch's half width at its start).
 * @param steps How many shades darker it makes the ground.
 * @param density How solid it is: above 1 the middle is solid and only the rim thins out.
 * @param bandTop, bandBottom Only the screen rows from bandTop up to (not including) bandBottom
 *        are darkened (a band of the view, see replayDrawList).
 * @param away The direction a stretched patch runs along the ground (x, y; a unit vector), or null for a disc.
 * @param length How far a stretched patch runs, in voxels.
 */
void FirstPersonView::drawGroundShade(const double centre[3], double radius, int steps, float density, int bandTop, int bandBottom,
	const double *away, double length)
{
	if (_gpuRecording)
	{
		gpuRecordGroundShade(centre, radius, steps, density, away, length);
		if (_gpuRecordOnly) return;
	}
	// The screen rectangle around it: the four corners of the box around the disc, or around the
	// stretched patch from its start to its end at its widest, projected (nothing drawn if any is
	// behind the camera), cut to the view.
	const bool stretched = away && length > 0.0;
	const double reach = stretched ? radius * FlareShadowWidening : radius;
	const double endX = stretched ? centre[0] + away[0] * length : centre[0], endY = stretched ? centre[1] + away[1] * length : centre[1];
	const int width = getWidth(), height = getHeight();
	int left = width, top = height, right = -1, bottom = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double cornerPoint[3] = { (corner & 1) ? std::max(centre[0], endX) + reach : std::min(centre[0], endX) - reach,
			(corner & 2) ? std::max(centre[1], endY) + reach : std::min(centre[1], endY) - reach, centre[2] + 0.5 };
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
			// How close to the middle (0 at the edge .. 1): of the disc; of a stretched patch,
			// across it (as wide as the disc at its start, FlareShadowWidening times at its end)
			// and fading along it (the disc under the unit covers its start).
			float closeness;
			if (stretched)
			{
				const double along = offsetX * away[0] + offsetY * away[1], aside = std::abs(offsetX * away[1] - offsetY * away[0]);
				if (along < 0.0 || along > length) continue;
				const double share = along / length, halfWidth = radius * (1.0 + (FlareShadowWidening - 1.0) * share);
				if (aside > halfWidth) continue;
				closeness = (float)((1.0 - aside / halfWidth) * (1.0 - share));
			}
			else
			{
				const double distanceFromCentre = std::sqrt(offsetX * offsetX + offsetY * offsetY);
				if (distanceFromCentre > radius) continue;
				closeness = (float)(1.0 - distanceFromCentre / radius);
			}
			// Its depth along the view axis, in true proportions like the depth buffer's.
			const double viewDepth = distanceAlongRay * (rayDirection[0] * _forward[0] + rayDirection[1] * _forward[1]
				+ rayDirection[2] * ZScale * _forward[2]); // true depth
			if (std::abs(_depthBuffer[pixelY * width + pixelX] - viewDepth) > 3.0) continue; // only on the floor right there
			// Darker towards the middle: more of the pixels get the shade (dithered), each
			// darkened within its colour group.
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
 * With the setting "See-through corpses" (off at first: the owner's), the pile pointed at (its
 * picture under the crosshair or the free cursor last frame, see findUnitAtPointer) is drawn
 * faint, at the ghost opacity (setting "Ghost opacity"), while the game's line through the
 * pointer meets something past it, not the floor it lies on (a player's report, GitHub issue
 * #35: very tall corpses, XPiratez's, hid what a shot reaches, though the game's shots pass
 * through items; the owner: the whole picture faint): what's behind it shows.
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
	// The pile pointed at past: faint, at the ghost opacity.
	_faintPile = { nullptr, std::clamp(Options::fpsGhostOpacity, 0, 100) / 100.0f };
	// The items are recorded first, then drawn on all the view's threads at once (see
	// replayDrawList); each one's frame is composed in the sprite buffer and kept till then.
	int framesUsed = 0;
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		// Only tiles with something on them, that have been seen.
		// (Not while a quick fall plays there, nor while a body coming down from the air hasn't
		// landed there: the body and what it dropped come once it's down.)
		Tile *tile = _save->getTile(tileIndex);
		if (tile->getInventory()->empty() || !_tileViews[tileIndex].discovered
			|| std::find(partsDrawn.begin(), partsDrawn.end(), tileIndex) != partsDrawn.end() || FirstPerson::quickFallOn(_save, tile)
			|| FirstPerson::airFallLandsOn(_save, tile))
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
		step.sprite.pile = tilesUnder[0];
		// Pointed at, and aimed past (the game's line meets something on another tile, farther off
		// than the pile itself): faint.
		if (Options::fpsSeeThroughCorpses && tilesUnder[0] == _pileAtPointer && _pointerAimed)
		{
			bool ownFloor = false;
			for (const Tile *under : tilesUnder) ownFloor = ownFloor || under->getPosition() == _pointerAim.toTile();
			const double pileDistance = std::hypot(ground[0] - _eye[0], ground[1] - _eye[1], ground[2] - _eye[2]);
			const double aimDistance = std::hypot(_pointerAim.x + 0.5 - _eye[0], _pointerAim.y + 0.5 - _eye[1], _pointerAim.z + 0.5 - _eye[2]);
			if (!ownFloor && aimDistance > pileDistance)
			{
				step.sprite.seeThrough = &_faintPile;
			}
		}
		// An unconscious unit lying in the pile (any of a big corpse's tiles): its picture can be
		// pointed at (a medi-kit's patient). A unit lying there hit a moment ago (or killed where it
		// lay): the pile flashes as a standing unit does when hit (settings "Hit flash" and "Muzzle
		// flashes"; a player, GitHub issue #74), at most once however many are hit.
		const Uint32 now = SDL_GetTicks();
		float flash = 0.0f;
		for (Tile *under : tilesUnder)
		{
			for (BattleItem *pileItem : *under->getInventory())
			{
				BattleUnit *lying = pileItem->getUnit();
				if (!lying)
				{
					continue;
				}
				if (!step.sprite.body && lying->getStatus() == STATUS_UNCONSCIOUS)
				{
					step.sprite.body = lying;
				}
				auto watched = _watchedUnits.find(lying);
				if (Options::fpsMuzzleFlash && Options::fpsHitFlash && watched != _watchedUnits.end() && watched->second.hitTick && now - watched->second.hitTick < HitFlinchTime)
				{
					flash = std::max(flash, 1.0f - (now - watched->second.hitTick) / (float)HitFlinchTime);
				}
			}
		}
		step.sprite.shade = (int)std::lround(std::clamp(shade - flash * HitFlashShade, 0.0f, 15.0f));
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
	const int factor = spriteUpscaleFactor();
	const Surface *bigFrom = upscaleHere(world, 1.0) ? upscaled(from, factor) : nullptr;
	const Surface *bigTo = bigFrom ? upscaled(to, factor) : nullptr;
	if (bigFrom && bigTo)
	{
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
		if (_gpuRecording)
		{
			gpuRecordDot(left, top, size, size, depth, shadeColor(_colors.edgeDot, (int)(_colors.ambient * 0.5f)), 0, 1.0f, 0, -1.0f, haze);
			if (_gpuRecordOnly) return;
		}
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
		// Only burning tiles (a burning tile has smoke too) that have been seen (a chain of
		// explosions' fire once its ripple has passed over the tile: see shownFire).
		Tile *tile = _save->getTile(tileIndex);
		if (!tile->getSmoke() || !shownFire(tileIndex, tile) || !_tileViews[tileIndex].discovered)
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
 * The colour of the game's smoke, from its frames for the thickest smoke: the colours of their
 * dots mixed and turned most of the way to grey (as the see-through smoke's are, see
 * softSmokeFrame), so the puffs, trails and dust drawn in it match the game's smoke.
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
	const Surface *smooth = Options::fpsSpriteUpscale > 0 ? upscaled(frame, UpscaleFactor) : nullptr;
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
/**
 * Notes each row's runs of coloured pixels in a frame of see-through smoke, once (it is never
 * changed after: an upgraded or recoloured frame is made anew): the smoke is drawn only along them
 * (see drawSprite), an original frame's dots leaving most of it empty and a soft frame's edges
 * fading out to nothing. Found on the drawing thread before the frame's puffs are drawn (see
 * drawSeeThroughSmoke).
 */
void FirstPersonView::SoftSmoke::findRuns() const
{
	if (!runStart.empty() || !colors)
	{
		return;
	}
	const SDL_Surface *surface = colors->getSurface();
	for (int y = 0; y < surface->h; ++y)
	{
		runStart.push_back((int)runs.size());
		const Uint8 *row = (const Uint8 *)surface->pixels + y * surface->pitch;
		for (int x = 0; x < surface->w;)
		{
			while (x < surface->w && !row[x]) ++x;
			const int first = x;
			while (x < surface->w && row[x]) ++x;
			if (x <= first) continue;
			// (A gap of a few pixels is drawn over: a run costs more than the pixels it would skip.)
			if (!runs.empty() && (int)runs.size() > runStart.back() && first - runs.back().second < SmokeRunGapMin) runs.back().second = x;
			else runs.push_back({ first, x });
		}
	}
	runStart.push_back((int)runs.size());
}

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
 * it (an enemy they don't see with smoke between isn't drawn, see drawUnits): the range in voxels, and how
 * much of the smoke and of the smoke over fire their eyes see through (the armor's heat vision,
 * in percent). The see-through smoke is drawn thinner by as much (a player: a scout drone's 50%
 * heat vision didn't look any better), down to SmokeSeenMin of itself (see drawSeeThroughSmoke).
 * @param sightVoxels Their sight range, in voxels.
 * @param throughSmoke How much of smoke they see through, in percent.
 * @param throughFire How much of smoke over fire they see through, in percent.
 */
/**
 * The most opaque see-through smoke may get where it hangs (the settings "Smoke opacity" and
 * "Night smoke opacity", a player: "non full smoke can be blinding for some players" at night
 * and on dark maps; since GitHub issue #98 a maximum: smoke over smoke builds up to it and no
 * further): the day value in the light, the night value in the dark. Dark is the darker of the
 * battle (a night mission, however lamps or the suit lights light the smoke: lit smoke on a dark
 * night is the blinding kind) and the game's shade where the smoke hangs (a dark base or an unlit
 * room by day; see SmokeNightFromShade). Only the smoke the soldier sees through: where their
 * sight runs out it stays solid.
 * @param shade The game's shade where the smoke hangs, 0 brightest .. 15 darkest.
 * @return The opacity, 0.05 .. 0.95.
 */
float FirstPersonView::smokeOpacityAt(int shade) const
{
	const float darkness = std::max(_colors.night, std::clamp((shade - SmokeNightFromShade) / (SmokeNightToShade - SmokeNightFromShade), 0.0f, 1.0f));
	const float day = std::clamp(Options::fpsSmokeOpacity, 5, 95) / 100.0f, night = std::clamp(Options::fpsNightSmokeOpacity, 5, 95) / 100.0f;
	return day + (night - day) * darkness;
}

void FirstPersonView::setSmokeSight(float sightVoxels, int throughSmoke, int throughFire)
{
	_smokeSightVoxels = sightVoxels;
	_smokeBlock = (100 - std::clamp(throughSmoke, 0, 100)) / 300.0f;
	_fireBlock = (100 - std::clamp(throughFire, 0, 100)) / 300.0f;
	_smokeSeen = std::max(SmokeSeenMin, (100 - std::clamp(throughSmoke, 0, 100)) / 100.0f);
}

/**
 * The smoke on discovered tiles drawn as the map draws the game's smoke, nothing solid where the
 * soldier's sight runs out (a player's report, GitHub issue #98; the owner: "0.8's way": the game
 * hides units in smoke, not the world). With the setting "Smoke" Modern, the game's own smoke made
 * soft (see softSmokeFrame), and the wisps rising off it, more opaque the thicker the smoke, up to
 * the most the settings "Smoke opacity" and "Night smoke opacity" allow, which smoke over smoke
 * builds up to and no further (see smokeOpacityAt, _smokeVeil); with Original, the original
 * frames' own dots as they are, solid, one frame a tile, as on the map (see stippleSmokeFrame),
 * and no wisps. Drawn after everything solid, nearest first; units the soldier sees stay clear
 * (see _unitVeil), the others aren't drawn with smoke between (see drawUnits).
 * Each tile's smoke is the frame the map picks for its density; its frames melt into each other slowly (an eased cross-fade, nothing blinks),
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
	// the two frames it is between, how far, how opaque (and the most its light allows, see
	// smokeOpacityAt), how big and how lit.
	struct Puff { double depth; double world[3]; int anchorY; const Surface *from, *to; double blend; float opacity, cap; double scale; int shade; };
	std::vector<Puff> puffs;
	auto addPuff = [&](const double world[3], int anchorY, int firstFrame, double frameTime, float opacity, float cap, double scale, int shade) {
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
		puff.cap = cap;
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
		// Set off from the middle; Modern's clouds sway and bob slowly, Original's stay put as on the
		// map (its solid dots would crawl a pixel at a time; its frames melt into each other instead).
		const double sway = original ? 0.0 : 1.0;
		const double world[3] = {
			ground[0] + (variation[0] - 0.5) * 4.0 + sway * 0.8 * std::sin(seconds * 0.5 + variation[1] * 2.0 * Pi),
			ground[1] + (variation[1] - 0.5) * 4.0 + sway * 0.8 * std::cos(seconds * 0.4 + variation[2] * 2.0 * Pi),
			ground[2] + sway * 1.0 * std::sin(seconds * 0.7 + variation[3] * 2.0 * Pi) };
		// Modern: a little more opaque the thicker the smoke (a fresh smoke grenade makes about
		// 15), the thickest as opaque as the settings "Smoke opacity" and "Night smoke opacity"
		// allow by the light here, their maximum, which smoke over smoke builds up to and no
		// further (a player's report, GitHub issue #98: stacked smoke hid more than the game's
		// sight rules do; see smokeOpacityAt, _smokeVeil). Original: the game's dots as they are,
		// solid, one frame a tile, as the map draws them (the owner: "exactly as the map"; their
		// holes are the see-through). Both thinner for eyes with heat vision (see setSmokeSight).
		const float cap = original ? 0.0f : _smokeSeen * smokeOpacityAt(_tileViews[tileIndex].light);
		const float opacity = original ? _smokeSeen : cap * std::clamp(0.45f + tile->getSmoke() * 0.025f, 0.0f, 0.85f) / 0.85f;
		const int firstFrame = firstSmokeFrame(tile->getSmoke());
		const double frameTime = seconds / FrameSeconds + tile->getAnimationOffset() + variation[2], scale = 1.14 + variation[3] * 0.12;
		addPuff(world, 32, firstFrame, frameTime, opacity, cap, scale, _tileViews[tileIndex].light);
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
		const float cap = _smokeSeen * smokeOpacityAt(shade);
		addPuff(world, 20, firstSmokeFrame(1), seconds / (FrameSeconds * 1.5) + particle.size, cap * (0.45f / 0.85f) * fade * fade * (3.0f - 2.0f * fade),
			cap, 0.35 + 0.4 * age, shade);
	}

	// Nearest first, so the smoke in front keeps its own shape and sway when the smoke behind
	// it would go past the most a pixel may take (see _smokeVeil): its two frames part way into each other, made
	// here (on this thread). An original frame upgraded to the upscaler's version on the way
	// drops the blends made with the old one, some of which may be in the list already: then the
	// list is made again (it happens once for each frame, ever).
	std::sort(puffs.begin(), puffs.end(), [](const Puff &a, const Puff &b) { return a.depth < b.depth; });
	unsigned upgradesBefore;
	do
	{
		upgradesBefore = _smokeUpgrades;
		_smokeDraws.clear();
		for (const Puff &puff : puffs)
		{
			const SoftSmoke &soft = softSmokeBlend(puff.from, puff.to, std::min(SmokeBlendSteps - 1, (int)(puff.blend * SmokeBlendSteps)), original);
			soft.findRuns();
			_smokeDraws.push_back({ &soft, { puff.world[0], puff.world[1], puff.world[2] }, puff.anchorY, puff.opacity, puff.cap, puff.scale, puff.shade });
		}
	} while (_smokeUpgrades != upgradesBefore);
	// Only what can show, each puff in the rows it may reach (see cullSmokeDraws); then drawn on
	// all the view's threads, each a band of rows at a time (see drawSmokeBands).
	if (_gpuRecording)
	{
		// (For the card, every puff once, whole: its own depth test leaves out what can't show.)
		for (const SmokeDraw &smoke : _smokeDraws)
		{
			const SeeThroughSprite seeThroughSprite = { smoke.soft->opacity.data(), smoke.opacity, nullptr, nullptr, nullptr, 0, (int)std::lround(smoke.cap * SeeThroughLevels) };
			SpriteDraw draw;
			draw.sprite = smoke.soft->colors;
			draw.anchorX = 16 * UpscaleFactor;
			draw.anchorY = smoke.anchorY * UpscaleFactor;
			std::copy(smoke.world, smoke.world + 3, draw.world);
			draw.depthBias = SmokeDepthBias;
			draw.shade = smoke.shade;
			draw.scale = smoke.scale / UpscaleFactor;
			draw.seeThrough = &seeThroughSprite;
			gpuRecordSprite(draw);
		}
		if (_gpuRecordOnly) return;
	}
	cullSmokeDraws();
	if (!_smokeDraws.empty())
	{
		const bool recording = _gpuRecording;
		_gpuRecording = false; // (the check: the CPU draws them too, on its threads, nothing recorded twice)
		runOnAllThreads(&FirstPersonView::drawSmokeBands);
		_gpuRecording = recording;
	}
}

/**
 * Leaves out of this frame's see-through smoke the puffs that can't show, and notes the screen
 * rows each of the others covers, so that each band of rows draws only the puffs in it (see
 * drawSmokeBands): every band sets each of its puffs' cards up anew, and a thick cloud has
 * hundreds of them (GitHub issue #92, "smoke grenades drawing maybe 70 tiles of smoke"; the
 * owner: "still draw all smoke but check if we can do it efficiently"). A puff is left out when it
 * lies off the view, or wholly behind what's drawn already (a wall, a unit), where every pixel of
 * it would fail the depth test: the picture stays the same. The card is placed as drawSprite places it (upright, leaning with the
 * view's pitch, more steeply above the eye) and its four corners taken to the screen: its rows and
 * columns there (a row more each way), and its nearest corner's depth against the farthest depth
 * drawn in each block of the view it covers. With the view tilted by a lean, or a corner behind
 * the camera, a puff may be anywhere and is kept, on every row.
 */
void FirstPersonView::cullSmokeDraws()
{
	if (_smokeDraws.empty())
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const int blockColumns = (width + SmokeCullBlock - 1) / SmokeCullBlock, blockRows = (height + SmokeCullBlock - 1) / SmokeCullBlock;
	// The farthest depth drawn in each block (the sky's is infinite: nothing in front of it hides).
	_smokeCullDepth.assign(blockColumns * blockRows, 0.0f);
	for (int pixelY = 0; pixelY < height; ++pixelY)
	{
		const float *depthRow = &_depthBuffer[pixelY * width];
		float *blockRow = &_smokeCullDepth[(pixelY / SmokeCullBlock) * blockColumns];
		for (int pixelX = 0; pixelX < width; ++pixelX)
		{
			float &farthest = blockRow[pixelX / SmokeCullBlock];
			farthest = std::max(farthest, depthRow[pixelX]);
		}
	}
	const double focalLength = (width / 2.0) / _halfFovTangent;
	const double level[2] = { std::cos(_yaw), std::sin(_yaw) };
	auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	auto cannotShow = [&](SmokeDraw &smoke)
	{
		smoke.top = 0;
		smoke.bottom = height;
		if (_roll != 0.0)
		{
			return false;
		}
		// The card as drawSmokeBands draws it (anchorX 16 and anchorY its own, in the frame's
		// pixels, at its scale), leaning as drawSprite leans it.
		const Surface *sprite = smoke.soft->colors;
		const double scale = smoke.scale / UpscaleFactor, anchorX = 16.0 * UpscaleFactor, anchorY = smoke.anchorY * UpscaleFactor;
		const double columnSize = scale * SpritePixelX, rowSize = scale * ZScale;
		const double anchor[3] = { smoke.world[0] - _eye[0], smoke.world[1] - _eye[1], (smoke.world[2] - _eye[2]) * ZScale };
		const double elevation = std::atan2(anchor[2], std::sqrt(anchor[0] * anchor[0] + anchor[1] * anchor[1]));
		double tilt = std::clamp(_pitch, -CardTiltMax * Pi / 180.0, CardTiltUpMax * Pi / 180.0);
		if (elevation > CardTiltUpMax * Pi / 180.0)
		{
			tilt = std::max(tilt, std::min(elevation - CardTiltUpMax * Pi / 180.0, CardTiltSteepMax * Pi / 180.0));
		}
		const double cardUp[3] = { -std::sin(tilt) * level[0], -std::sin(tilt) * level[1], std::cos(tilt) };
		// Its corners on the screen.
		double left = width, right = 0.0, top = height, bottom = 0.0, nearest = Infinity;
		for (const double up : { anchorY * rowSize, (anchorY - sprite->getHeight()) * rowSize })
			for (const double across : { -anchorX * columnSize, (sprite->getWidth() - anchorX) * columnSize })
			{
				const double corner[3] = { anchor[0] + cardUp[0] * up + _levelRight[0] * across, anchor[1] + cardUp[1] * up + _levelRight[1] * across,
					anchor[2] + cardUp[2] * up + _levelRight[2] * across };
				const double depth = dot(corner, _forward);
				if (depth <= 1.0)
				{
					return false;
				}
				const double screenX = width / 2.0 + dot(corner, _right) * focalLength / depth, screenY = height / 2.0 - dot(corner, _up) * focalLength / depth;
				left = std::min(left, screenX); right = std::max(right, screenX);
				top = std::min(top, screenY); bottom = std::max(bottom, screenY);
				nearest = std::min(nearest, depth);
			}
		const int firstColumn = std::max(0, (int)std::floor(left) - 1), endColumn = std::min(width, (int)std::ceil(right) + 1);
		smoke.top = std::max(0, (int)std::floor(top) - 1);
		smoke.bottom = std::min(height, (int)std::ceil(bottom) + 1);
		if (firstColumn >= endColumn || smoke.top >= smoke.bottom)
		{
			return true;
		}
		// Hidden when nothing drawn in the blocks it covers lies farther than its nearest pixel would.
		const float nearestPixel = (float)std::max(1.0, nearest - SmokeDepthBias);
		for (int blockY = smoke.top / SmokeCullBlock; blockY <= (smoke.bottom - 1) / SmokeCullBlock; ++blockY)
			for (int blockX = firstColumn / SmokeCullBlock; blockX <= (endColumn - 1) / SmokeCullBlock; ++blockX)
				if (_smokeCullDepth[blockY * blockColumns + blockX] > nearestPixel) return false;
		return true;
	};
	_smokeDraws.erase(std::remove_if(_smokeDraws.begin(), _smokeDraws.end(), cannotShow), _smokeDraws.end());
}

/**
 * Draws this frame's see-through smoke (made by drawSeeThroughSmoke) over bands of the view's
 * rows, taken one at a time from the shared counter until none are left: a job for all the
 * view's threads (see runOnAllThreads). Each band gets every puff, nearest first, so every
 * pixel comes out just as if one thread drew the puffs whole, one after another.
 */
void FirstPersonView::drawSmokeBands()
{
	const int bandCount = (getHeight() + BandRows - 1) / BandRows;
	for (int band = _rayWorkers.nextPiece++; band < bandCount; band = _rayWorkers.nextPiece++)
	{
		for (const SmokeDraw &smoke : _smokeDraws)
		{
			if (smoke.bottom <= band * BandRows || smoke.top >= (band + 1) * BandRows) continue; // (not in this band: see cullSmokeDraws)
			const SeeThroughSprite seeThroughSprite = { smoke.soft->opacity.data(), smoke.opacity, smoke.soft->runStart.data(), smoke.soft->runs.data(),
				_smokeCullDepth.data(), (getWidth() + SmokeCullBlock - 1) / SmokeCullBlock, (int)std::lround(smoke.cap * SeeThroughLevels) };
			SpriteDraw draw;
			draw.sprite = smoke.soft->colors;
			draw.anchorX = 16 * UpscaleFactor;
			draw.anchorY = smoke.anchorY * UpscaleFactor;
			std::copy(smoke.world, smoke.world + 3, draw.world);
			draw.depthBias = SmokeDepthBias;
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
 * Marks the visible enemies like the iso map's indicators do: an arrow over the head with the
 * enemy's number, drawn over everything (it's what the soldier knows, not what's in view), kept at
 * the screen edge when the enemy is off to the side; red, or in the colour of how the soldier knows
 * it (setting "Enemy markers": green as its button when only the squad sees it, yellow when his
 * line to it is clear, see MarkerKind). Our own the map flags the same way (setting "Wounded
 * markers"), in their buttons' blue and purple; a unit with two buttons has its markers stacked, as
 * the game stacks the buttons (each MarkerStackStep over the one before, the first at the bottom).
 * The pixel art is drawn bigger on big screens (the UI scale).
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
		const Uint8 markerColor = spot.kind == MarkerSquadSeen ? _colors.markerSquadSeen : spot.kind == MarkerLineOfFire ? _colors.markerLineOfFire
			: spot.kind == MarkerWounded ? _colors.markerWounded : spot.kind == MarkerPassingOut ? _colors.markerPassingOut : _colors.marker;
		// Where the unit's head is on the view (no marker if it's behind the camera), kept
		// inside the view with room for the art around it (all of its stack's): the arrow's tip,
		// this marker's place in the stack over it.
		double screenX, screenY, depth;
		if (!projectToScreen(spot.pos, screenX, screenY, depth))
		{
			continue;
		}
		const int stackRoom = std::min((14 + (spot.stackSize - 1) * MarkerStackStep) * uiScale, height - 4 * uiScale);
		const int tipX = std::clamp((int)screenX, 6 * uiScale, width - 7 * uiScale),
			tipY = std::clamp((int)screenY, stackRoom, height - 4 * uiScale) - spot.stack * MarkerStackStep * uiScale;
		scaleOriginX = tipX;
		scaleOriginY = tipY;
		// An arrow pointing down, 7 wide at the top, outlined: first a black triangle one pixel
		// bigger all round (one more row on top, every row one wider on each side), then the
		// coloured triangle over it, then a black pixel under its tip.
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int row = pass == 0 ? -1 : 0; row < 4; ++row)
			{
				const int halfWidth = 3 - std::max(row, 0) + (pass == 0 ? 1 : 0);
				for (int offset = -halfWidth; offset <= halfWidth; ++offset)
				{
					put(tipX + offset, tipY - 4 + row, pass == 0 ? _colors.tapeBlack : markerColor);
				}
			}
		}
		put(tipX, tipY, _colors.tapeBlack);
		// The enemy's number above it, in the tiny 3x5 digits (4 pixels apart), centred: first
		// a black outline around every lit dot, then the coloured dots.
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
							put(dotX, dotY, markerColor);
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
 * on big screens, on the body's near half (hanging from its middle on the screen: a unit standing
 * right behind the body, its feet higher up the screen, doesn't wear them on its legs; a player,
 * GitHub issue #116), but only where the body itself is in view (not behind a wall, nor in smoke
 * too thick to see through) and within the view's reach.
 * With "Crosshair", only for the body the player points at, just under the crosshair, where the
 * shot info is read too, strictly by what the crosshair, or the free cursor, is on, nearest first
 * (#116; the owner: "strictly pointing at the body if it is in-front of enemy behind it shows body
 * icons"): the body whose own picture is right under it, nearer than anything else drawn there,
 * even with the line of fire passing it to a unit standing behind (that unit's name shows, the
 * shot would hit it); none while a standing unit is pointed at otherwise (the one named there, see
 * FirstPersonMode::updatePointedUnit: a body in front of it made a fine unit look as if it bled
 * and burned); else the body whose picture is within a few pixels (see findUnitAtPointer), as a
 * melee blow and the medi-kit take it; else one far off (its tile narrower than
 * DownedFarTileShare of the view: its picture only a few pixels) within about a tile of where the
 * player points. A body drawn faint there ("See-through corpses": the line passes it) isn't
 * what's pointed at. A unit whose mod switches its indicators off gets none.
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
	// it, how far it is from where the player points (in about a tile's breadth on the screen
	// there), that breadth, and whether its picture is the one pointed at.
	struct Downed { double screenX, screenY; int states; double fromPointer, tileOnScreen; bool pictured; };
	std::vector<Downed> downed;
	// The body pointed at by its picture: with a standing unit pointed at too, only one whose
	// picture is right under the pointer (on top there: drawn nearer); else the one nearest it.
	const BattleUnit *pointedBody = _aimedUnit ? _pointerBodies[PointerReach][PointerReach] : _bodyAtPointer;
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (tile->getInventory()->empty() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		int states = 0;
		bool pictured = false;
		for (const BattleItem *item : *tile->getInventory())
		{
			const BattleUnit *unit = item->getUnit();
			if (!unit || unit->getStatus() != STATUS_UNCONSCIOUS || !unit->indicatorsAreEnabled()) continue;
			pictured = pictured || unit == pointedBody;
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
			gpuNeedDepth();
			inView = _depthBuffer[pixelY * width + pixelX] >= depth - 8.0;
			break;
		}
		if (!inView)
		{
			continue;
		}
		// The icons hanging from its middle on the screen, their outline's top there (whatever
		// stands behind the body stands higher up the screen), kept on the view seen close by.
		const double tileOnScreen = std::max(1.0, 12.0 * focalLength / depth);
		const double bottom = std::min(screenY + 8.0 * uiScale, (double)(height - uiScale));
		downed.push_back({ screenX, bottom, states, std::hypot(screenX - _pointerX, screenY - _pointerY) / tileOnScreen, tileOnScreen, pictured });
	}
	// With "Crosshair", only the body pointed at, its icons just under the crosshair (which
	// reaches about 8 pixels of the art round where it points), below any name shown there:
	// the body by its picture (see pointedBody); none while a standing unit is pointed at
	// otherwise (its name there, the icons would read as its own); else one far off, within
	// about a tile.
	if (Options::fpsDownedIcons == 1)
	{
		const double farTile = DownedFarTileShare * width;
		auto nearest = [&](bool byPicture) {
			const Downed *best = nullptr;
			for (const Downed &body : downed)
			{
				const bool candidate = byPicture ? body.pictured : body.tileOnScreen < farTile && body.fromPointer <= 1.0;
				if (candidate && (!best || body.fromPointer < best->fromPointer)) best = &body;
			}
			return best;
		};
		const Downed *pointedAt = nearest(true);
		if (!pointedAt && !_aimedUnit) pointedAt = nearest(false);
		if (!pointedAt)
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

/**
 * The numbers rising over the units hit (setting "Damage numbers", see
 * FirstPersonDamageNumbers.cpp): each from the point just over the unit's head where it was hit,
 * in the world: tested against the depth of what's drawn, so a wall or a unit in front hides it as
 * it would anything there, but at a fixed size on the screen (the game's small font, as the HUD's
 * text), so it reads far off too; within the view's reach. They rise from just over where the
 * enemy marker's arrow and number go (drawn over everything after them), so neither hides the
 * other. Numbers that would overlap step up over the newer ones (see placeDamageNumbers).
 */
void FirstPersonView::drawDamageNumbers()
{
	if (!_damageNumbers || _damageNumbers->numbers().empty())
	{
		return;
	}
	const std::vector<FirstPerson::DamageNumber> &numbers = _damageNumbers->numbers();
	const Font *font = _game->getMod()->getFont("FONT_SMALL");
	const FirstPerson::DamageNumberColors colors = FirstPerson::damageNumberColors(getPalette());
	const Uint32 now = SDL_GetTicks();
	const int uiScale = std::max(1, _uiScale);
	const int markerClearance = 14; // (the marker's art reaches 13 pixels over the head, see drawMarkers)
	// Where each one's point is on the view, and how far ahead (none behind the camera or past the view's reach).
	std::vector<FirstPerson::DamageNumberSpot> spots(numbers.size());
	std::vector<double> depths(numbers.size(), 0.0);
	for (size_t index = 0; index < numbers.size(); ++index)
	{
		double screenX = 0.0, screenY = 0.0;
		const bool shown = projectToScreen(numbers[index].head, screenX, screenY, depths[index]) && depths[index] <= _maxDistance;
		spots[index] = { (int)std::lround(screenX), (int)std::lround(screenY) - markerClearance * uiScale, shown };
	}
	// Clear of the crosshair and the texts round it (drawn over the view after it: a unit struck
	// right under the crosshair, its number rises over the crosshair rather than behind them): the
	// crosshair's picture, the unit's name under it (see FirstPersonMode::updateUnitName), as wide
	// as a name.
	std::vector<FirstPerson::DamageNumberBox> keepClear;
	if (_pointerX >= 0 && _pointerY >= 0)
	{
		keepClear.push_back({ _pointerX - 24 * uiScale, _pointerX + 24 * uiScale, _pointerY - 11 * uiScale, _pointerY + 10 * uiScale + _pointerTextBelow });
	}
	const std::vector<int> bottoms = FirstPerson::placeDamageNumbers(numbers, spots, font, uiScale, now, keepClear);
	for (size_t index = 0; index < numbers.size(); ++index)
	{
		if (spots[index].shown)
		{
			gpuNeedDepth();
			FirstPerson::drawDamageNumber(this, font, numbers[index], spots[index].x, bottoms[index], uiScale, now, colors, _depthBuffer.data(), depths[index]);
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
