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
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
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
 * @param sprite The sprite (palette colours, 0 transparent).
 * @param anchorX The sprite's pixel column that stands at the point.
 * @param anchorY The sprite's pixel row that stands at the point.
 * @param world The point, in voxels.
 * @param depthBias How much nearer than the point the sprite counts for the depth test, in
 *                  voxels (so it isn't hidden by the floor it stands on).
 * @param shade Palette shades to darken it by (0 none .. 15).
 * @param scale World size per sprite pixel (an upscaled sprite is drawn at 1 / its factor).
 */
void FirstPersonView::drawSprite(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade,
	double scale)
{
	// Where the anchor lands on the view, and how far away it is.
	double anchorScreenX, anchorScreenY, viewDepth;
	if (!sprite || !projectToScreen(world, anchorScreenX, anchorScreenY, viewDepth))
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	// Screen pixels per sprite pixel: isometric pixels are 1/sqrt(2) of a voxel across and one voxel up.
	const double pixelsPerColumn = (width / 2.0) / _halfFovTangent / viewDepth * scale * SpritePixelX;
	const double pixelsPerRow = (width / 2.0) / _halfFovTangent / viewDepth * scale * ZScale;
	// The screen rectangle the sprite covers, cut to the view (right and bottom exclusive).
	const int spriteWidth = sprite->getWidth(), spriteHeight = sprite->getHeight();
	const int left = std::max(0, (int)std::floor(anchorScreenX - anchorX * pixelsPerColumn));
	const int right = std::min(width, (int)std::ceil(anchorScreenX + (spriteWidth - anchorX) * pixelsPerColumn));
	const int top = std::max(0, (int)std::floor(anchorScreenY - anchorY * pixelsPerRow));
	const int bottom = std::min(height, (int)std::ceil(anchorScreenY + (spriteHeight - anchorY) * pixelsPerRow));
	const float spriteDepth = (float)std::max(1.0, viewDepth - depthBias);
	// Right in front of the camera a sprite is a wall of huge pixels: fade it out (dithered),
	// but not a unit on the next tile (melee range), which stays solid.
	const float opacity = (float)std::clamp((viewDepth - 4.0) / 6.0, 0.0, 1.0);
	if (opacity <= 0.0f)
	{
		return;
	}
	if (right <= left || bottom <= top)
	{
		return;
	}
	// Which sprite column each screen column shows (-1 outside the sprite), worked out once
	// (close up a sprite covers much of the screen); rows are read straight from the sprite.
	_spriteColumns.resize(right - left);
	for (int pixelX = left; pixelX < right; ++pixelX)
	{
		const int spriteX = (int)std::floor(anchorX + (pixelX + 0.5 - anchorScreenX) / pixelsPerColumn);
		_spriteColumns[pixelX - left] = spriteX >= 0 && spriteX < spriteWidth ? spriteX : -1;
	}
	// Copy the sprite's opaque pixels, shaded, where nothing nearer is drawn already, and
	// note their depth so that farther sprites drawn later are hidden by them.
	const SDL_Surface *spriteSurface = const_cast<Surface*>(sprite)->getSurface(); // read only
	for (int pixelY = top; pixelY < bottom; ++pixelY)
	{
		const int spriteY = (int)std::floor(anchorY + (pixelY + 0.5 - anchorScreenY) / pixelsPerRow);
		if (spriteY < 0 || spriteY >= spriteHeight) continue;
		const Uint8 *spriteRow = (const Uint8 *)spriteSurface->pixels + spriteY * spriteSurface->pitch;
		Uint8 *screenRow = getRaw(0, pixelY);
		float *depthRow = &_depthBuffer[pixelY * width];
		for (int pixelX = left; pixelX < right; ++pixelX)
		{
			const int spriteX = _spriteColumns[pixelX - left];
			if (spriteX < 0) continue;
			const Uint8 color = spriteRow[spriteX];
			if (!color || spriteDepth >= depthRow[pixelX] || (opacity < 1.0f && dither(pixelX, pixelY) >= opacity)) continue;
			screenRow[pixelX] = shadeColor(color, shade);
			depthRow[pixelX] = spriteDepth;
		}
	}
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
 * The unit being looked through is not drawn.
 */
void FirstPersonView::drawUnits()
{
	// The game's 8-frame animation cycle (a frame every 100 ms), and the sprite drawer, with
	// the diving helmets on under the sea.
	const int animFrame = (SDL_GetTicks() / 100) % 8;
	UnitSprite unitSprite(_spriteBuffer, _game->getMod(), _save, animFrame, _save->getDepth() != 0, 0, 0);
	const bool debug = _save->getDebugMode();
	_markerSpots.clear();
	// Time since the last frame in seconds (at most a tenth, none the first time), for the gliding.
	const Uint32 now = SDL_GetTicks();
	const double elapsedSeconds = _unitLastTick ? std::min<Uint32>(100, now - _unitLastTick) / 1000.0 : 0.0;
	_unitLastTick = now;

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
			drawUnitShadow(feet, 5.5 * unitSize); // flying units too: the shadow on the ground shows where they hover
		}
		// Draw the unit's frame into the sprite buffer, then stand it in the world.
		const GraphSubset mask(SpriteBufferSize, SpriteBufferSize);
		// (Drawn unlit and shaded when placed: an upscaled frame then serves in any light.)
		if (unitSize == 1)
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			drawSpriteSmooth(_spriteBuffer, 32, 40, feet, 0.0, shade); // anchored at the centre of the floor diamond
		}
		else
		{
			// The parts are laid out on screen like the 2x2 tiles they cover, back to front.
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
			unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
			unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
			drawSpriteSmooth(_spriteBuffer, 32, 48, feet, 0.0, shade); // anchored at the corner shared by the four tiles
		}
	}
}

/**
 * A soft dark patch on the ground under a unit, so it stands on the floor instead of floating.
 * @param feet The point on the ground under the unit's middle, in voxels.
 * @param radius The patch's radius, in voxels.
 */
void FirstPersonView::drawUnitShadow(const double feet[3], double radius)
{
	drawGroundShade(feet, radius, 2, 1.8f);
}

/**
 * Darkens the ground in a soft disc (unit shadows, scorched earth): pixels whose floor lies
 * within the disc lose up to `steps` shades, thinning out (dithered) towards the edge. Only
 * pixels showing the floor right there are darkened, not a wall or a unit in front of it.
 * @param centre The disc's centre on the ground, in voxels.
 * @param radius The disc's radius, in voxels.
 * @param steps How many shades darker it makes the ground.
 * @param density How solid it is: above 1 the middle is solid and only the rim thins out.
 */
void FirstPersonView::drawGroundShade(const double centre[3], double radius, int steps, float density)
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
	left = std::max(0, left); top = std::max(0, top); right = std::min(width - 1, right); bottom = std::min(height - 1, bottom);
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
			getRayDirection(pixelX, pixelY, rayDirection);
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
 * Draws the top item lying on each discovered tile (weapons, corpses, ...) as a sprite on the
 * floor, lit by the game's light just above it.
 */
void FirstPersonView::drawFloorItems()
{
	const int animFrame = (SDL_GetTicks() / 100) % 8;
	ItemSprite itemSprite(_spriteBuffer, _game->getMod(), _save, animFrame);
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		// Only tiles with something on them, that have been seen.
		Tile *tile = _save->getTile(tileIndex);
		if (tile->getInventory()->empty() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		BattleItem *item = tile->getTopItem();
		if (!item)
		{
			continue;
		}
		// The middle of the tile's floor; skip it if it's behind the camera or far off the sides.
		const Position tilePosition = tile->getPosition();
		const double ground[3] = { tilePosition.x * 16.0 + 8.0, tilePosition.y * 16.0 + 8.0,
			tilePosition.z * 24.0 - tile->getTerrainLevel() };
		double screenX, screenY, depth;
		if (!projectToScreen(ground, screenX, screenY, depth) || screenX < -200 || screenX > getWidth() + 200)
		{
			continue;
		}
		// Draw the item's floor sprite into the buffer (unlit) and stand it on the floor.
		_spriteBuffer->clear();
		const int shade = (int)std::lround(std::clamp(lightAt((float)ground[0], (float)ground[1], (float)ground[2] + 4.0f), 0.0f, 15.0f));
		itemSprite.draw(item, 16, 8, 0);
		drawSpriteSmooth(_spriteBuffer, 32, 40, ground, 0.0, shade);
	}
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
 * Smoke and fire on discovered tiles, animated like on the isometric map: the same sprites
 * from SMOKE.PCK, picked the same way (fire, or smoke by its density), with the frames dissolving
 * into each other.
 */
void FirstPersonView::drawSmokeAndFire()
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
		// Only tiles with smoke (a burning tile has smoke too) that have been seen.
		Tile *tile = _save->getTile(tileIndex);
		if (!tile->getSmoke() || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		// Fire is the set's first four frames, unshaded (it shines). Smoke is further on (its
		// own frames under the sea), thicker smoke further still, as the map picks it for the
		// mod's smoke layout; smoke is shaded by the tile's light.
		int firstFrame = 0, shade = 0;
		if (!tile->getFire())
		{
			firstFrame += _save->getDepth() > 0 ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
			if (Mod::EXTENDED_SMOKE_OFFSET == 0)
				firstFrame += int(std::floor((tile->getSmoke() / 6.0) - 0.1));
			else if (Mod::EXTENDED_SMOKE_OFFSET == 1)
				firstFrame += int(std::floor((tile->getSmoke() / 6.0) - 0.1)) * 4;
			else
				firstFrame += (tile->getSmoke() - 1) / 5 * 4;
			shade = _tileViews[tileIndex].light;
		}
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

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
