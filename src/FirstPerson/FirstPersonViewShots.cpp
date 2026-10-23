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
 * FirstPersonViewShots.cpp: shots in flight and what they leave behind.
 *
 * Bullets, beams, plasma blobs, rockets with their smoke and thrown items; our own shots leaving
 * the muzzle of the weapon in hand; dual wield's second round; impacts and explosions' sprites;
 * bullet holes and scorch marks; and debris from destroyed tile parts.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <SDL.h>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Script.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Battlescape/Explosion.h"
#include "../Battlescape/ItemSprite.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Position.h"
#include "../Battlescape/Projectile.h"
#include "FirstPersonParticles.h"
#include "FirstPersonViewShared.h"
#include "FirstPersonVoxelModel.h"

namespace OpenXcom
{

/**
 * The voxel model of an item's kind (see FirstPersonVoxelModel.h): its inventory picture (from
 * BIGOBS.PCK, as the inventory and the weapon in hand show it) given volume, made the first time
 * it's asked for and kept. A kind with no picture gets an empty model.
 * @param item An item of the kind.
 * @return Its model: the faces, and the middle of the picture's opaque pixels.
 */
const FirstPersonView::ItemModel &FirstPersonView::itemModel(BattleItem *item)
{
	const RuleItem *rules = item->getRules();
	auto found = _itemModels.find(rules);
	if (found != _itemModels.end())
	{
		return found->second;
	}
	ItemModel &model = _itemModels[rules];
	model.middle[0] = model.middle[1] = 0.0f;
	SurfaceSet *inventorySprites = _game->getMod()->getSurfaceSet("BIGOBS.PCK");
	if (!inventorySprites)
	{
		return model;
	}

	// The picture, drawn as the inventory draws it (its recolouring scripts), whole at the
	// frame's own place (the hand slot would centre it by the item's size and could push a
	// picture drawn wider than the item off the frame), then given volume.
	if (!_itemPicture)
	{
		_itemPicture = new Surface(32, 48, 0, 0);
	}
	_itemPicture->clear();
	if (const Surface *frame = item->getBigSprite(inventorySprites, _save, 0))
	{
		ScriptWorkerBlit worker;
		BattleItem::ScriptFill(&worker, item, _save, BODYPART_ITEM_INVENTORY, 0, 0);
		worker.executeBlit(frame, _itemPicture, 0, 0, 0);
	}
	voxelizePicture(_itemPicture, model.faces, pictureIsPlate(_game->getMod()->getFirstPerson(), item->getRules(), _itemPicture));

	// The middle of its opaque pixels: the point that flies along the game's path.
	int minX = INT_MAX, maxX = -1, minY = INT_MAX, maxY = -1;
	for (int y = 0; y < _itemPicture->getHeight(); ++y)
	{
		for (int x = 0; x < _itemPicture->getWidth(); ++x)
		{
			if (!_itemPicture->getPixel(x, y)) continue;
			minX = std::min(minX, x); maxX = std::max(maxX, x);
			minY = std::min(minY, y); maxY = std::max(maxY, y);
		}
	}
	if (maxX >= 0)
	{
		model.middle[0] = (minX + maxX) / 2.0f;
		model.middle[1] = (minY + maxY) / 2.0f;
	}
	return model;
}

/**
 * Draws a thrown item in flight as its voxel model, at its position on its path: level with the
 * path to start with (the picture's right along it, its depth across it), turning end over end
 * about the axis across the path (ThrownItemSpin turns a second), ThrownItemScale voxels per
 * picture pixel. Its faces that look at the eye are drawn far to near, depth-tested
 * against the world (walls and nearer things hide it), each darkened by how far it turns from
 * the light above and by the light where it is (the game's, lamps, the flashlight and flares, as
 * on units).
 * The work is done in true proportions (heights scaled by ZScale), like the camera.
 * @param current Where it is, in voxels.
 * @param before Where it was a moment ago (which way it flies on the level).
 * @param item The item thrown.
 * @param thrownStart When it left the hand (SDL ticks), for its tumble.
 * @return False if the item has no voxel model (it's then drawn as its sprite).
 */
bool FirstPersonView::drawThrownItem(const Position &current, const Position &before, BattleItem *item, Uint32 thrownStart)
{
	const ItemModel &model = itemModel(item);
	if (model.faces.empty())
	{
		return false;
	}

	// Where it is, and which way it flies on the level.
	const double centre[3] = { (double)current.x, (double)current.y, current.z * ZScale };
	double path[2] = { (double)(current.x - before.x), (double)(current.y - before.y) };
	const double pathLength = std::sqrt(path[0] * path[0] + path[1] * path[1]);
	if (pathLength < 1e-6)
	{
		path[0] = 1.0; path[1] = 0.0; // straight up or down: any level direction will do
	}
	else
	{
		path[0] /= pathLength; path[1] /= pathLength;
	}

	// Its axes in the world, each one picture pixel (or voxel of depth) long: right along the
	// path, down towards the ground, depth across the path; the first two turned end over end.
	const double angle = (SDL_GetTicks() - thrownStart) / 1000.0 * ThrownItemSpin * 2.0 * Pi;
	const double along[3] = { path[0], path[1], 0.0 }, upward[3] = { 0.0, 0.0, 1.0 };
	double stepPerX[3], stepPerY[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		stepPerX[axis] = (along[axis] * std::cos(angle) + upward[axis] * std::sin(angle)) * ThrownItemScale;
		stepPerY[axis] = (along[axis] * std::sin(angle) - upward[axis] * std::cos(angle)) * ThrownItemScale;
	}
	const double stepPerDepth[3] = { path[1] * ThrownItemScale, -path[0] * ThrownItemScale, 0.0 };

	// A mirror image turns the faces' windings round: the sign of the map's determinant tells.
	const double determinant = stepPerX[0] * (stepPerY[1] * stepPerDepth[2] - stepPerY[2] * stepPerDepth[1])
		- stepPerX[1] * (stepPerY[0] * stepPerDepth[2] - stepPerY[2] * stepPerDepth[0])
		+ stepPerX[2] * (stepPerY[0] * stepPerDepth[1] - stepPerY[1] * stepPerDepth[0]);
	const double windingSign = determinant < 0.0 ? -1.0 : 1.0;

	// The light on it, as on units, and where the light comes from (above, a little to the side).
	const float point[3] = { (float)current.x, (float)current.y, (float)current.z };
	const int shade = (int)std::lround(std::clamp(lightAt(point[0], point[1], point[2]) - pointLight(point, nullptr, false)
		- flareLight(point, nullptr, false) * 8.0f, 0.0f, 15.0f));
	const double towardsLight[3] = { 0.30, -0.35, 0.89 };

	// The eye in true proportions, and the view's focal length in pixels.
	const double eye[3] = { _eye[0], _eye[1], _eye[2] * ZScale };
	const double focalLength = (getWidth() / 2.0) / _halfFovTangent;

	// Each face that shows: on the screen, with its shaded colour and its distance ahead.
	struct Quad { float depth; double screen[4][2]; Uint8 color; };
	std::vector<Quad> quads;
	quads.reserve(model.faces.size() / 2);
	for (const VoxelFace &face : model.faces)
	{
		// Its corners in the world, and ahead of / right of / above the eye.
		double worldCorners[4][3], ahead[4], toRight[4], above[4];
		bool tooClose = false;
		for (int cornerIndex = 0; cornerIndex < 4 && !tooClose; ++cornerIndex)
		{
			const double pictureX = face.corner[cornerIndex][0] - model.middle[0], pictureY = face.corner[cornerIndex][1] - model.middle[1];
			const double depth = face.corner[cornerIndex][2];
			ahead[cornerIndex] = toRight[cornerIndex] = above[cornerIndex] = 0.0;
			for (int axis = 0; axis < 3; ++axis)
			{
				worldCorners[cornerIndex][axis] = centre[axis] + pictureX * stepPerX[axis] + pictureY * stepPerY[axis] + depth * stepPerDepth[axis];
				const double relative = worldCorners[cornerIndex][axis] - eye[axis];
				ahead[cornerIndex] += relative * _forward[axis];
				toRight[cornerIndex] += relative * _right[axis];
				above[cornerIndex] += relative * _up[axis];
			}
			tooClose = ahead[cornerIndex] < 4.0; // right in front of the camera, as sprites
		}
		if (tooClose)
		{
			continue;
		}

		// Its outward normal and its centre: drawn only if it turns towards the eye.
		double edge1[3], edge2[3], faceNormal[3], faceCentre[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			edge1[axis] = worldCorners[1][axis] - worldCorners[0][axis];
			edge2[axis] = worldCorners[3][axis] - worldCorners[0][axis];
			faceCentre[axis] = (worldCorners[0][axis] + worldCorners[2][axis]) * 0.5;
		}
		faceNormal[0] = (edge1[1] * edge2[2] - edge1[2] * edge2[1]) * windingSign;
		faceNormal[1] = (edge1[2] * edge2[0] - edge1[0] * edge2[2]) * windingSign;
		faceNormal[2] = (edge1[0] * edge2[1] - edge1[1] * edge2[0]) * windingSign;
		if (faceNormal[0] * (faceCentre[0] - eye[0]) + faceNormal[1] * (faceCentre[1] - eye[1]) + faceNormal[2] * (faceCentre[2] - eye[2]) >= 0.0)
		{
			continue;
		}

		// Shading: up to 3 shades darker as it turns from the light, plus the light where it is
		// (within the colour's group of 16 shades).
		const double normalLength = std::sqrt(faceNormal[0] * faceNormal[0] + faceNormal[1] * faceNormal[1] + faceNormal[2] * faceNormal[2]);
		const double lighting = std::max(0.0,
			(faceNormal[0] * towardsLight[0] + faceNormal[1] * towardsLight[1] + faceNormal[2] * towardsLight[2]) / normalLength);
		Quad quad;
		quad.color = shadeColor(face.color, (int)std::lround((1.0 - lighting) * 3.0) + shade);
		quad.depth = (float)((ahead[0] + ahead[2]) * 0.5);
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
		{
			quad.screen[cornerIndex][0] = getWidth() / 2.0 + toRight[cornerIndex] * focalLength / ahead[cornerIndex];
			quad.screen[cornerIndex][1] = getHeight() / 2.0 - above[cornerIndex] * focalLength / ahead[cornerIndex];
		}
		quads.push_back(quad);
	}

	// Far to near, each behind whatever is nearer in the world.
	std::sort(quads.begin(), quads.end(), [](const Quad &first, const Quad &second) { return first.depth > second.depth; });
	for (const Quad &quad : quads)
	{
		fillQuadAtDepth(quad.screen, quad.color, quad.depth);
	}
	return true;
}

/**
 * Fills a convex quadrilateral on the view, row by row (a scanline fill): every pixel whose
 * centre lies inside it and where nothing nearer is drawn yet, noting its depth there.
 * @param corners The four corners on the view, in pixels, in order around the edge.
 * @param color Palette colour to fill with.
 * @param depth Its distance ahead of the camera (the depth buffer's measure).
 */
void FirstPersonView::fillQuadAtDepth(const double corners[4][2], Uint8 color, float depth)
{
	const int width = getWidth(), height = getHeight();

	// The rows it covers: those whose pixel centres lie between its top and bottom corners.
	double top = corners[0][1], bottom = corners[0][1];
	for (int cornerIndex = 1; cornerIndex < 4; ++cornerIndex)
	{
		top = std::min(top, corners[cornerIndex][1]);
		bottom = std::max(bottom, corners[cornerIndex][1]);
	}
	const int firstRow = std::max(0, (int)std::ceil(top - 0.5)), lastRow = std::min(height - 1, (int)std::floor(bottom - 0.5));
	for (int y = firstRow; y <= lastRow; ++y)
	{
		// Where the row's centre line crosses the edges: the leftmost and rightmost crossing.
		const double rowCentreY = y + 0.5;
		double leftX = 1e9, rightX = -1e9;
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
		{
			const double *edgeStart = corners[cornerIndex], *edgeEnd = corners[(cornerIndex + 1) % 4];
			if ((edgeStart[1] <= rowCentreY && edgeEnd[1] > rowCentreY) || (edgeEnd[1] <= rowCentreY && edgeStart[1] > rowCentreY))
			{
				const double crossX = edgeStart[0] + (rowCentreY - edgeStart[1]) / (edgeEnd[1] - edgeStart[1]) * (edgeEnd[0] - edgeStart[0]);
				leftX = std::min(leftX, crossX);
				rightX = std::max(rightX, crossX);
			}
		}

		// The pixels between the two crossings that nothing nearer covers.
		const int firstColumn = std::max(0, (int)std::ceil(leftX - 0.5)), lastColumn = std::min(width - 1, (int)std::floor(rightX - 0.5));
		Uint8 *screenRow = getRaw(0, y);
		float *depthRow = &_depthBuffer[y * width];
		for (int x = firstColumn; x <= lastColumn; ++x)
		{
			if (depth >= depthRow[x]) continue;
			screenRow[x] = color;
			depthRow[x] = depth;
		}
	}
}

/**
 * Draws a Doom free grenade in flight (see setFreeGrenade): tumbling like a thrown item, where it
 * is now: moved on along its velocity from where it was last set (at most a twentieth of a second
 * on, the flight's own steps come about that often), so it flies smoothly between them. One
 * with no inventory picture to give it a model isn't drawn.
 */
void FirstPersonView::drawFreeGrenade()
{
	if (!_freeGrenade)
	{
		return;
	}
	const float seconds = std::min(0.05f, (SDL_GetTicks() - _freeGrenadeTick) / 1000.0f);
	const Position current((int)std::floor(_freeGrenadePos[0] + _freeGrenadeVelocity[0] * seconds), (int)std::floor(_freeGrenadePos[1] + _freeGrenadeVelocity[1] * seconds),
		(int)std::floor(_freeGrenadePos[2] + _freeGrenadeVelocity[2] * seconds));
	const Position before((int)std::floor(current.x - _freeGrenadeVelocity[0] * 0.05f), (int)std::floor(current.y - _freeGrenadeVelocity[1] * 0.05f), current.z);
	drawThrownItem(current, before, _freeGrenade, _freeGrenadeStart);
}

/**
 * Draws what flies and what explodes, as sprites and dots in the world: a thrown item (its own
 * sprite, at the game's position), the head of a shot in flight (a dot, blob or flame with its
 * streak; beams are drawn as lines by drawBeams), dual wield's second round, and the explosions with the
 * game's own animations (X1.PCK for big ones, HIT.PCK for hits, SMOKE.PCK for puffs), each
 * frame dissolving smoothly into the next.
 */
void FirstPersonView::drawProjectilesAndExplosions()
{
	if (!_map)
	{
		return;
	}
	Mod *mod = _game->getMod();
	// A Doom free grenade in flight (the 3D view's own throw).
	drawFreeGrenade();
	// The game's one projectile in flight.
	if (Projectile *projectile = _map->getProjectile())
	{
		if (BattleItem *item = projectile->getItem())
		{
			// A thrown item: as its voxel model, tumbling; one with no inventory picture as its
			// sprite as the map draws it, put into the sprite buffer at (16, 8) and stood at the
			// item's position in the world.
			if (!drawThrownItem(projectile->getPosition(0), projectile->getPosition(-4), item, _thrownStart))
			{
				const Position position = projectile->getPosition();
				const double world[3] = { (double)position.x, (double)position.y, (double)position.z };
				ItemSprite itemSprite(_spriteBuffer, mod, _save, _save->getAnimFrame());
				_spriteBuffer->clear();
				itemSprite.draw(item, 16, 8, 0);
				drawSpriteSmooth(_spriteBuffer, 32, 34, world, 4.0, 0); // like Map: the voxel sits at sprite pixel (16, 26)
			}
		}
		else if (_shotTrail != TRAIL_BEAM) // beams are drawn as lines
		{
			// The head of the shot: a small dot for bullets, a glowing blob for plasma, a flame for
			// rockets, with its streak behind it, all as big as its calibre.
			float head[3];
			shotHead(projectile, head);
			drawShotDot(head, _shotTrailOrigin, _shotTrail, _shotTrailColor, _shotCalibre);
		}
	}
	if (_map->getPartnerProjectile() && _partnerTrail != TRAIL_BEAM)
	{
		drawShotDot(_partnerHead, _partnerOrigin, _partnerTrail, _partnerColor, _partnerCalibre); // dual wield: the other gun's round
	}
	// Explosions step through their frames on the game's clock; each frame dissolves into the
	// next over the time frames have been lasting. First forget the clocks of explosions that
	// are over.
	const Uint32 now = SDL_GetTicks();
	for (auto entry = _explosionClocks.begin(); entry != _explosionClocks.end(); )
	{
		const auto &explosions = *_map->getExplosions();
		entry = std::find(explosions.begin(), explosions.end(), entry->first) == explosions.end()
			? _explosionClocks.erase(entry) : std::next(entry);
	}
	for (const Explosion *explosion : *_map->getExplosions())
	{
		const int frame = explosion->getCurrentFrame();
		if (frame < 0)
		{
			continue; // not started yet
		}
		// When the frame changes, note how long the last one lasted (15..250 ms): the next
		// frame takes as long to dissolve in. The blend is how far into that dissolve we are.
		FrameClock &clock = _explosionClocks.try_emplace(explosion, FrameClock{ frame, now, 0 }).first->second;
		if (frame != clock.frame)
		{
			clock.length = std::clamp<Uint32>(now - clock.since, 15, 250);
			clock.frame = frame;
			clock.since = now;
		}
		const double blend = clock.length ? std::min(1.0, (now - clock.since) / (double)clock.length) : 0.0;
		// The game's sprite for this frame and the next: none after the explosion's own last
		// frame (the frames after it in the set are another animation's: a bullet's hit fading
		// into the start of another weapon's looked like a second explosion).
		const Position position = explosion->getPosition();
		const double world[3] = { (double)position.x, (double)position.y, (double)position.z };
		SurfaceSet *spriteSet = mod->getSurfaceSet(explosion->isBig() ? "X1.PCK" : explosion->isHit() ? "HIT.PCK" : "SMOKE.PCK");
		const Surface *sprite = spriteSet->getFrame(frame);
		if (!sprite)
		{
			continue;
		}
		const Surface *nextSprite = frame + 1 <= explosion->getLastFrame() && frame + 1 < (int)spriteSet->getTotalFrames() ? spriteSet->getFrame(frame + 1) : nullptr;
		// Anchored where the isometric map anchors them: a big explosion by its centre, a hit
		// at (15, 25), a puff at (15, 15). They are depth-tested as if 24 (big) or 12 voxels
		// nearer, so the walls and floor they burst against don't cut them.
		if (explosion->isBig())
		{
			drawSpriteBlend(sprite, nextSprite, blend, sprite->getWidth() / 2, sprite->getHeight() / 2, world, 24.0, 0);
		}
		else
		{
			drawSpriteBlend(sprite, nextSprite, blend, 15, explosion->isHit() ? 25 : 15, world, 12.0, 0);
		}
	}
}

/**
 * Leaves a mark where a shot hit: a bullet hole, or a bigger scorch from energy weapons and
 * incendiaries. Only the 400 newest marks are kept.
 * @param pos Where the shot hit, in voxels.
 * @param size How big the mark is (1 for a bullet hole, 3 for a scorch); on the screen it
 * shrinks with distance.
 * @param darkness How many shades it darkens the surface.
 */
void FirstPersonView::addDecal(const float pos[3], int size, int darkness)
{
	_decals.push_back({ { pos[0], pos[1], pos[2] }, (Uint8)size, (Uint8)darkness });
	if (_decals.size() > 400)
	{
		_decals.erase(_decals.begin());
	}
}

/**
 * Bullet holes and scorch marks on whatever surface is there, and blackened ground around
 * explosions. Drawn before the units, which stand in front of them.
 */
void FirstPersonView::drawDecals()
{
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // in pixels
	// Scorched ground around big explosions: up to four shades darker in a soft disc.
	for (const auto &scorch : _scorches)
	{
		const double centre[3] = { scorch.pos[0], scorch.pos[1], scorch.pos[2] };
		drawGroundShade(centre, scorch.radius, 4, 1.4f);
	}
	// Each bullet hole or scorch: a dark round spot, at most 6 pixels in radius, on the screen
	// where it is (skip it if it's behind the camera or right in front of it).
	for (const auto &decal : _decals)
	{
		const double world[3] = { decal.pos[0], decal.pos[1], decal.pos[2] };
		double screenX, screenY, depth;
		if (!projectToScreen(world, screenX, screenY, depth) || depth < 4.0) continue;
		const int radius = std::clamp((int)std::lround(decal.size * focalLength / depth * 0.3), 0, 6); // in pixels
		for (int y = (int)screenY - radius; y <= (int)screenY + radius; ++y)
		{
			for (int x = (int)screenX - radius; x <= (int)screenX + radius; ++x)
			{
				if (x < 0 || y < 0 || x >= width || y >= height) continue;
				// Only within the disc, and only on the surface it hit (the scene there is within
				// 2.5 voxels of the mark's depth): a unit or a wall in front stays clean.
				const double distance = std::sqrt((x - screenX) * (x - screenX) + (y - screenY) * (y - screenY));
				if (distance > radius + 0.5 || std::abs(_depthBuffer[y * width + x] - depth) > 2.5) continue;
				if (radius > 1 && distance > radius * 0.6 && dither(x, y) > 0.5f) continue; // ragged edge
				// Darker by the mark's darkness (decal.depth, in shades), within the pixel's own
				// colour group (empty black pixels stay as they are).
				Uint8 &pixel = *getRaw(x, y);
				if (pixel) pixel = (Uint8)((pixel & 0xF0) | std::min(15, (pixel & 15) + decal.depth));
			}
		}
	}
}

/**
 * Something on a tile was destroyed (blown up, burnt, shot away): chunks of it fly, in its
 * own colours, and a cloud of dust rises.
 *
 * The particles are thrown with FirstPersonParticles::burst, whose arguments are, in order:
 * where, how many, kind, group, the colours and how many of them, the slowest and fastest
 * speed (voxels per second), the upward bias of the directions, gravity (voxels per second
 * squared, negative rises), drag (share of the speed lost per second), the shortest and
 * longest life (seconds), and the size (pixels at one tile's distance).
 * @param tilePosition The tile it stood on.
 * @param part Which part of the tile it was (O_FLOOR, O_WESTWALL, O_NORTHWALL, O_OBJECT).
 * @param destroyedMapData What it was, before it was destroyed.
 */
void FirstPersonView::spawnDebris(const Position &tilePosition, int part, const MapData *destroyedMapData)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// The chunks' colours: two of the destroyed part's own, picked going up through its voxel
	// model every third layer, in the middle of its solid box (a wall stands along the tile's
	// west or north edge, so the tile's middle is empty), else the first solid voxel anywhere
	// in that layer of the box (a window or a doorway may leave the middle open); dust and
	// smoke colours for any not found.
	const VoxelModel *model = getModel(destroyedMapData);
	Uint8 chunkColors[2] = { _colors.dust, _colors.smoke };
	const int boxMiddleX = (model->min[0] + model->max[0]) / 2, boxMiddleY = (model->min[1] + model->max[1]) / 2;
	for (int z = model->min[2], foundCount = 0; z <= model->max[2] && foundCount < 2; z += 3)
	{
		Uint8 color = model->color[z][boxMiddleY][boxMiddleX];
		for (int y = model->min[1]; !color && y <= model->max[1]; ++y)
		{
			for (int x = model->min[0]; !color && x <= model->max[0]; ++x)
			{
				color = model->color[z][y][x];
			}
		}
		if (color) chunkColors[foundCount++] = color;
	}
	// From where the piece stood (in voxels): walls on the tile's west or north edge, anything
	// else in the middle of the tile; halfway up its solid voxels.
	const float centre[3] = { tilePosition.x * 16.0f + (part == O_WESTWALL ? 1.0f : 8.0f),
		tilePosition.y * 16.0f + (part == O_NORTHWALL ? 1.0f : 8.0f),
		tilePosition.z * 24.0f + (model->min[2] + model->max[2]) * 0.5f };
	// Chunks: thrown out hard (20..80 voxels per second, mostly upwards), falling fast and
	// gone within two seconds.
	_particles.burst(centre, 24, P::LIT, P::EFFECT, chunkColors, 2, 20, 80, 0.6f, 200, 0.4f, 0.8f, 1.8f, 3);
	// Dust: big slow puffs, rising and hanging for a few seconds.
	const Uint8 dustColors[2] = { _colors.dust, _colors.smoke };
	_particles.burst(centre, 10, P::LIT, P::EFFECT, dustColors, 2, 3, 14, 0.5f, -8, 1.2f, 1.5f, 3.0f, 6);
}

/**
 * What a shot leaves behind along the bit of its path it flew this frame: nothing for bullets,
 * embers for incendiary rounds, smoke for rockets and other explosives (and white specks that
 * hang along the path for half a minute, on land and under the sea), green glow for plasma,
 * sparkles for stun, droplets for acid. Under the sea every shot (and thrown thing) also leaves
 * bubbles that hang in the water for half a minute. The look is _shotTrail, _shotTrailColor and
 * _shotCalibre (spawnPartnerEffects swaps dual wield's second round's in for its own trail).
 * @param from Where the shot was drawn last frame, in voxels.
 * @param to Where it is drawn now, in voxels.
 */
void FirstPersonView::spawnTrailSegment(const float from[3], const float to[3])
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// The bit of path, in voxels. Nothing for a shot that hardly moved, or one that jumped
	// more than 400 voxels (not a real flight).
	const float segment[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	const float length = std::sqrt(segment[0] * segment[0] + segment[1] * segment[1] + segment[2] * segment[2]);
	if (length < 0.5f || length > 400.0f)
	{
		return;
	}
	// A point part way along it (0 at the start .. 1 at the end).
	auto along = [&](float progress, float out[3])
	{
		for (int axis = 0; axis < 3; ++axis) out[axis] = from[axis] + segment[axis] * progress;
	};
	// One particle for about every 3 voxels flown (a rocket's smoke every 1.5), each at a random
	// spot within its share of the path. A rocket's lasting specks only while there's room for
	// them under SpeckRoom particles in all, so a salvo of rockets leaves the rest of the
	// battle's effects room (the system holds 6000).
	const int steps = std::max(1, (int)(length / (_shotTrail == TRAIL_SMOKE ? 1.5f : 3.0f)));
	const size_t SpeckRoom = 4500;
	const bool specksFit = _particles.all().size() < SpeckRoom;
	for (int step = 0; step < steps; ++step)
	{
		float point[3];
		along((step + _particles.random()) / steps, point);
		P::Particle particle;
		std::copy(point, point + 3, particle.pos);
		particle.age = 0; particle.gravity = 0; particle.drag = 0; particle.group = P::EFFECT;
		// The particle by the kind of trail (a thrown item leaves none).
		bool keep = true;
		switch (_lastProjectileThrown ? TRAIL_NONE : _shotTrail)
		{
		case TRAIL_FIRE:
			// Embers: drifting up a little, glowing in the hotter fire colours for half a second.
			particle.vel[0] = _particles.random(-3, 3); particle.vel[1] = _particles.random(-3, 3);
			particle.vel[2] = _particles.random(2, 8);
			particle.life = _particles.random(0.3f, 0.7f); particle.color = _colors.fire[(int)_particles.random(0, 2.99f)];
			particle.size = 2; particle.kind = P::GLOW;
			break;
		case TRAIL_SMOKE:
			// Smoke: puffs in two greys, starting a little behind the flame at the head (a rocket
			// flying away would have its newest puffs between it and the eye, over its flame),
			// slowing down and hanging for two to four seconds, bigger for bigger rounds (a
			// rocket's bigger than a cannon shell's).
			for (int axis = 0; axis < 3; ++axis) particle.pos[axis] -= segment[axis] / length * 5.0f;
			particle.vel[0] = _particles.random(-2, 2); particle.vel[1] = _particles.random(-2, 2);
			particle.vel[2] = _particles.random(1, 4);
			particle.life = _particles.random(2.0f, 4.0f);
			particle.color = _particles.random() < 0.6f ? _colors.smoke : shadeColor(_colors.smoke, 2);
			particle.size = (Uint8)std::lround(5.0f + 2.0f * _shotCalibre); particle.kind = P::LIT;
			particle.drag = 0.8f;
			// And two white specks about the spot, hanging there for half a minute, drifting a
			// little and greying as they go: the rocket's lasting trail.
			for (int speckIndex = 0; speckIndex < (specksFit ? 2 : 0); ++speckIndex)
			{
				P::Particle speck = particle;
				for (float &coordinate : speck.pos) coordinate += _particles.random(-0.6f, 0.6f);
				speck.vel[0] = _particles.random(-0.4f, 0.4f); speck.vel[1] = _particles.random(-0.4f, 0.4f);
				speck.vel[2] = _particles.random(0.1f, 0.5f);
				speck.life = _particles.random(25.0f, 35.0f); speck.drag = 0.2f;
				speck.color = _colors.speck; speck.size = 3;
				_particles.add(speck);
			}
			break;
		case TRAIL_PLASMA:
			// Plasma: a glow scattering in every direction, gone within half a second.
			particle.vel[0] = _particles.random(-4, 4); particle.vel[1] = _particles.random(-4, 4);
			particle.vel[2] = _particles.random(-4, 4);
			particle.life = _particles.random(0.2f, 0.5f); particle.color = _shotTrailColor; particle.size = 2; particle.kind = P::GLOW;
			break;
		case TRAIL_STUN:
			// Stun: tiny blinking sparkles, darting about for a moment.
			particle.vel[0] = _particles.random(-6, 6); particle.vel[1] = _particles.random(-6, 6);
			particle.vel[2] = _particles.random(-6, 6);
			particle.life = _particles.random(0.15f, 0.35f); particle.color = _shotTrailColor; particle.size = 1; particle.kind = P::BLINK;
			break;
		case TRAIL_ACID:
			// Acid: droplets falling off the path (about every other one).
			particle.vel[0] = _particles.random(-2, 2); particle.vel[1] = _particles.random(-2, 2); particle.vel[2] = 0;
			particle.life = _particles.random(0.4f, 0.8f); particle.gravity = 90;
			particle.color = _shotTrailColor; particle.size = 2; particle.kind = P::LIT;
			keep = _particles.random() < 0.5f;
			break;
		default:
			keep = false; // bullets, darts, harpoons, beams (drawn as lines): no trail
			break;
		}
		if (keep) _particles.add(particle);
		// Under the sea, every other step also leaves a bubble that rises slowly and hangs in
		// the water for half a minute.
		if (_colors.underwater && (step % 2 == 0))
		{
			P::Particle bubble;
			along((step + _particles.random()) / steps, bubble.pos);
			for (float &coordinate : bubble.pos) coordinate += _particles.random(-1.0f, 1.0f);
			bubble.vel[0] = _particles.random(-0.6f, 0.6f); bubble.vel[1] = _particles.random(-0.6f, 0.6f);
			bubble.vel[2] = _particles.random(0.8f, 2.5f);
			bubble.age = 0; bubble.life = _particles.random(22.0f, 34.0f); bubble.gravity = 0; bubble.drag = 0.05f;
			bubble.color = _colors.bubble; bubble.size = (Uint8)(1 + (int)(_particles.random() * 2.0f)); // 1..2
			bubble.kind = P::BUBBLE; bubble.group = P::EFFECT;
			_particles.add(bubble);
		}
	}
}

/**
 * Draws a line in the world (a laser beam, a shot's streak), depth-tested, faded by dithering;
 * it glows (its pixels feed the bloom), or with glows off (a hitbox's outline, the path of a
 * throw: setting "Glowing outlines") it's a plain line. It's a set number of pixels thick, or,
 * given a width in the world, as thick as that width looks at each point of it (thicker up
 * close, at least a pixel, at most the set number).
 * @param from, to The line's ends, in voxels.
 * @param color Its palette colour.
 * @param thickness How thick it is on the screen, in pixels (with a width in the world: at most).
 * @param opacity 1 solid .. 0 gone (dithered): a beam fading out.
 * @param worldWidth Its width in the world, in voxels (0: none, the set thickness all along).
 * @param bandTop, bandBottom Only the screen rows from bandTop up to (not including) bandBottom
 *        are drawn (a band of the view, see replayDrawList): the same pixels as drawn whole.
 * @param glows Whether its pixels shine (feed the bloom).
 */
void FirstPersonView::drawLine3D(const float from[3], const float to[3], Uint8 color, int thickness, float opacity, float worldWidth, int bandTop, int bandBottom, bool glows)
{
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // in pixels
	// Clip the line against a near plane 3 voxels in front of the eye (depth along the view
	// direction, in true proportions), so a beam from right beside the camera still draws.
	double start[3] = { from[0], from[1], from[2] }, end[3] = { to[0], to[1], to[2] };
	auto depthOf = [&](const double point[3])
	{
		return (point[0] - _eye[0]) * _forward[0] + (point[1] - _eye[1]) * _forward[1] + (point[2] - _eye[2]) * ZScale * _forward[2];
	};
	const double nearZ = 3.0, startDepth = depthOf(start), endDepth = depthOf(end);
	if (startDepth < nearZ && endDepth < nearZ) return;
	if (startDepth < nearZ || endDepth < nearZ)
	{
		// Move the end behind the near plane to where the line crosses it.
		const double cut = (nearZ - startDepth) / (endDepth - startDepth); // how far along the line it crosses
		double *clipped = startDepth < nearZ ? start : end;
		for (int axis = 0; axis < 3; ++axis) clipped[axis] = start[axis] + (end[axis] - start[axis]) * cut;
	}
	// Both ends on the screen (x, y in pixels, and depth); one pixel step at a time from one to
	// the other, giving up on absurdly long lines.
	double startX, startY, startZ, endX, endY, endZ;
	if (!projectToScreen(start, startX, startY, startZ) || !projectToScreen(end, endX, endY, endZ)) return;
	const int steps = std::max(1, (int)std::max(std::abs(endX - startX), std::abs(endY - startY)));
	if (steps > 4000) return;
	for (int step = 0; step <= steps; ++step)
	{
		const double progress = (double)step / steps;
		const int x = (int)(startX + (endX - startX) * progress), y = (int)(startY + (endY - startY) * progress);
		const float depth = (float)(1.0 / (1.0 / startZ + (1.0 / endZ - 1.0 / startZ) * progress)); // depth is linear in 1/z on screen
		// How thick it is here: the set thickness, or its width in the world as it looks here.
		const int pointThickness = worldWidth > 0.0f ? std::clamp((int)std::lround(worldWidth * focalLength / depth), 1, thickness) : thickness;
		// A square of that many pixels across around the point; in front of the scene (with a
		// voxel to spare, so a beam grazing a wall still shows), dithered by opacity.
		for (int offsetY = 0; offsetY < pointThickness; ++offsetY)
		{
			for (int offsetX = 0; offsetX < pointThickness; ++offsetX)
			{
				const int pixelX = x + offsetX - pointThickness / 2, pixelY = y + offsetY - pointThickness / 2;
				if (pixelX < 0 || pixelY < std::max(0, bandTop) || pixelX >= width || pixelY >= std::min(height, bandBottom)
					|| depth >= _depthBuffer[pixelY * width + pixelX] + 1.0f) continue;
				if (opacity < 1.0f && dither(pixelX, pixelY) >= opacity) continue;
				*getRaw(pixelX, pixelY) = color;
				_glowMask[pixelY * width + pixelX] = glows ? 1 : 0;
			}
		}
	}
}

/**
 * How big a shot and its muzzle flash are drawn, its calibre: 1 for a rifle's, less for a
 * pistol's, more for a heavy weapon's, so pistol, rifle, auto-cannon and heavy cannon (and laser
 * pistol, laser rifle and heavy laser...) each fire a bigger round than the last. It's the
 * kind of gun, by how it's carried (in one hand 0.75, a rifle in two hands 1, a heavy weapon
 * two squares wide or built in 1.25), times the square root of the round's power against a
 * rifle's of the same kind (bullets 30, lasers 60, plasma 80, explosives, fire, stun and acid
 * 60): 0.5 to 2.
 * @param weapon What fired it (null if unknown).
 * @param ammo What it fired (null: the weapon itself, a laser).
 * @return The calibre.
 */
float FirstPersonView::shotCalibre(const RuleItem *weapon, const RuleItem *ammo)
{
	const RuleItem *round = ammo ? ammo : weapon;
	if (!round)
	{
		return 1.0f;
	}
	float build = 1.0f;
	if (weapon && (weapon->isFixed() || weapon->getInventoryWidth() >= 2))
	{
		build = 1.25f;
	}
	else if (weapon && !weapon->isTwoHanded())
	{
		build = 0.75f;
	}
	const RuleDamageType *damage = round->getDamageType();
	float riflePower = 30.0f; // bullets and the like
	switch (damage ? damage->ResistType : DT_NONE)
	{
	case DT_PLASMA: riflePower = 80.0f; break;
	case DT_LASER: case DT_HE: case DT_IN: case DT_STUN: case DT_ACID: riflePower = 60.0f; break;
	default: break;
	}
	return std::clamp(build * std::sqrt(std::max(1, round->getPower()) / riflePower), 0.5f, 2.0f);
}

namespace
{

/**
 * Which skills a weapon's shots are aimed with (its accuracy multiplier): does the firing skill
 * count, the throwing skill, any other? The original games' default (not set by a mod): firing.
 * @param weapon The weapon.
 * @param firing Gets whether the firing skill counts.
 * @param throwing Gets whether the throwing skill counts.
 * @param other Gets whether any other stat counts (melee, strength, reactions...).
 */
void aimingSkills(const RuleItem *weapon, bool &firing, bool &throwing, bool &other)
{
	const RuleStatBonus *accuracy = weapon->getAccuracyMultiplierRaw();
	firing = !accuracy || !accuracy->isModded();
	throwing = other = false;
	if (firing)
	{
		return;
	}
	for (const auto &bonus : *accuracy->getBonusRaw())
	{
		const bool counts = std::any_of(bonus.second.begin(), bonus.second.end(), [](float coefficient) { return coefficient != 0.0f; });
		if (bonus.first == "firing") firing = firing || counts;
		else if (bonus.first == "throwing") throwing = throwing || counts;
		else other = other || counts;
	}
}

} // namespace

/**
 * Is a weapon launched by hand, no powder in it: a bow, a blowpipe, javelins, a whip's lash? The
 * big mods aim those with the throwing skill and none of the firing skill (the weapon's accuracy
 * multiplier), and the original games' guns all use firing. (Crossbows and the like aim with
 * firing: see flyingShot.)
 * @param weapon The weapon (null: no).
 * @return True if its shots are aimed with the throwing skill and not the firing skill.
 */
bool FirstPersonView::handLaunched(const RuleItem *weapon)
{
	if (!weapon)
	{
		return false;
	}
	bool firing, throwing, other;
	aimingSkills(weapon, firing, throwing, other);
	return throwing && !firing;
}

/**
 * Does a shot fly as an object, with no powder in it (an arrow, a dart, a bolt, a javelin)? It
 * has no muzzle flash, lights nothing up and barely kicks; and it flies as a white streak
 * (TRAIL_FLYING) unless what it carries has a look of its own (a burning arrow: see trailFor).
 * The item table (`firstPerson: items:`, shot: object or powder) decides first, the weapon's
 * entry, then the ammo's. Otherwise it's a shot from a weapon launched by hand (see
 * handLaunched), or one whose projectile picture (the ammo's, else the weapon's, as the game
 * picks it) the mod drew for those: a mod draws an arrow once, so a crossbow firing the bows'
 * arrow fires an arrow too. A picture counts when more of the weapons firing it are aimed by
 * throwing alone than by the firing skill (the big mods reuse their pictures widely: a whip's
 * lash, a firecracker and a rifle's round may share one, while the bows' arrow is fired mostly
 * by bows).
 * @param ammo The ammo fired (null if unknown).
 * @param weapon The weapon that fired it (null if unknown).
 * @return True for an object flying.
 */
bool FirstPersonView::flyingShot(const RuleItem *ammo, const RuleItem *weapon)
{
	// The item table first: the weapon's entry, then the ammo's.
	const RuleFirstPerson &table = _game->getMod()->getFirstPerson();
	for (const RuleItem *item : { weapon, ammo })
	{
		const RuleFirstPerson::ItemLook *look = item ? table.getItemLook(item->getType()) : nullptr;
		if (look && look->shot != RuleFirstPerson::SHOT_AUTO) return look->shot == RuleFirstPerson::SHOT_OBJECT;
	}
	if (handLaunched(weapon))
	{
		return true;
	}
	// The pictures drawn for objects, found once: each picture's ranged weapons (by the pictures
	// of the rounds they fire) counted, those aimed by throwing alone against those aimed with
	// the firing skill.
	if (!_flyingSpritesFound)
	{
		_flyingSpritesFound = true;
		std::map<int, int> thrownVotes, firedVotes;
		const Mod *mod = _game->getMod();
		for (const std::string &type : mod->getItemsList())
		{
			const RuleItem *rules = mod->getItem(type);
			if (!rules || rules->getBattleType() != BT_FIREARM) continue;
			if (rules->getCostAimed().Time <= 0 && rules->getCostSnap().Time <= 0 && rules->getCostAuto().Time <= 0) continue; // no shots (a melee weapon on ammo)
			bool firing, throwing, other;
			aimingSkills(rules, firing, throwing, other);
			std::set<int> sprites;
			const std::vector<const RuleItem*> *ammoTypes = rules->getPrimaryCompatibleAmmo();
			if (!ammoTypes || ammoTypes->empty())
			{
				sprites.insert(rules->getBulletSprite()); // it fires itself
			}
			else
			{
				for (const RuleItem *ammoType : *ammoTypes)
				{
					sprites.insert(ammoType->getBulletSprite() != Mod::NO_SURFACE ? ammoType->getBulletSprite() : rules->getBulletSprite());
				}
			}
			for (int sprite : sprites)
			{
				if (sprite == Mod::NO_SURFACE) continue;
				if (firing) ++firedVotes[sprite];
				else if (throwing && !other) ++thrownVotes[sprite];
			}
		}
		for (const auto &[sprite, votes] : thrownVotes)
		{
			if (votes > firedVotes[sprite]) _flyingSprites.insert(sprite);
		}
	}
	const int sprite = ammo && ammo->getBulletSprite() != Mod::NO_SURFACE ? ammo->getBulletSprite() : weapon ? weapon->getBulletSprite() : Mod::NO_SURFACE;
	return sprite != Mod::NO_SURFACE && _flyingSprites.count(sprite) > 0;
}

/**
 * How a shot looks in flight, from its ammo's damage type: the kind of trail and its colour.
 * Incendiary: embers; high explosive: smoke; laser: a red beam; plasma: a green blob; stun:
 * pale blue sparkles; acid: yellow-green droplets; anything else (bullets and the like): no
 * trail, a spark-coloured dot. An object flying (an arrow, a dart, see flyingShot) that would
 * look like a bullet or a rocket: a white streak (one carrying fire, acid, a shock keeps that
 * look: a burning arrow trails embers). Under the sea lasers are a pale blue, plasma a pale cyan, and
 * darts, harpoons and shells the vivid yellow of the original game's (pale ones would melt into
 * the sand).
 * The colours are the nearest the current palette has. And how big it is (see shotCalibre).
 * @param ammo The ammo fired (null if unknown).
 * @param weapon The weapon that fired it (null if unknown).
 * @param trail Gets the kind of trail.
 * @param color Gets its palette colour.
 * @param calibre Gets how big it's drawn.
 */
void FirstPersonView::trailFor(const BattleItem *ammo, const BattleItem *weapon, Trail &trail, Uint8 &color, float &calibre)
{
	const RuleDamageType *damage = ammo ? ammo->getRules()->getDamageType() : nullptr;
	// The palette colour nearest to a red, green, blue colour (getPalette only reads, but it
	// isn't marked const).
	const SDL_Color *palette = const_cast<FirstPersonView*>(this)->getPalette();
	auto nearest = [palette](int red, int green, int blue) {
		int best = 1, bestDistance = INT_MAX;
		for (int index = 1; index < 256; ++index)
		{
			const int dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
			if (dr * dr + dg * dg + db * db < bestDistance) { bestDistance = dr * dr + dg * dg + db * db; best = index; }
		}
		return (Uint8)best;
	};
	calibre = shotCalibre(weapon ? weapon->getRules() : nullptr, ammo ? ammo->getRules() : nullptr);
	switch (damage ? damage->ResistType : DT_NONE)
	{
	case DT_IN: trail = TRAIL_FIRE; color = _colors.fire[1]; break;
	case DT_HE: trail = TRAIL_SMOKE; color = _colors.smoke; break;
	case DT_LASER: trail = TRAIL_BEAM; color = _colors.underwater ? nearest(90, 220, 255) : nearest(255, 40, 30); break;
	case DT_PLASMA: trail = TRAIL_PLASMA; color = _colors.underwater ? nearest(180, 240, 255) : nearest(90, 255, 90); break;
	case DT_STUN: trail = TRAIL_STUN; color = nearest(160, 190, 255); break;
	case DT_ACID: trail = TRAIL_ACID; color = nearest(190, 255, 60); break;
	default: trail = TRAIL_NONE; color = _colors.underwater ? nearest(244, 240, 20) : _colors.spark; break;
	}
	// An object flying (an arrow, a bolt: see flyingShot) that would look like a bullet or a
	// rocket: a white streak.
	if ((trail == TRAIL_NONE || trail == TRAIL_SMOKE) && flyingShot(ammo ? ammo->getRules() : nullptr, weapon ? weapon->getRules() : nullptr))
	{
		trail = TRAIL_FLYING;
		color = nearest(245, 245, 245);
	}
}

/**
 * Where a shot ended, if not in a unit (that bleeds or sparks, see watchUnits): sparks, chunks
 * in the colour of what was hit and a puff of dust, and a mark: a bullet hole, or a bigger
 * scorch from beams, plasma and incendiaries. All of it on the surface the view shows there: the
 * game's line of fire can stop on collision voxels the art leaves empty (a craft's walls are
 * thicker than they look), so the shot's line is followed on a little through the view's own
 * world (from 3 voxels back to 12 on), and the mark goes where it meets what is drawn.
 * @param shotEnd Where the game's shot ended, in voxels.
 * @param from Where the shot came from, for its direction.
 * @param trail How it looked in flight (from its damage type).
 */
void FirstPersonView::spawnShotImpact(const float shotEnd[3], const float from[3], Trail trail)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// Nothing here if there is a unit in the tile it ended in, or in the tile below (a tall
	// unit reaches up into the next level).
	const Position endTile((int)shotEnd[0] / 16, (int)shotEnd[1] / 16, (int)shotEnd[2] / 24);
	const Tile *tile = _save->getTile(endTile);
	if (tile && (tile->getUnit() || (endTile.z > 0 && _save->getTile(endTile - Position(0, 0, 1))->getUnit())))
	{
		return;
	}
	// Where the shot's line meets what the view draws (see above).
	float head[3] = { shotEnd[0], shotEnd[1], shotEnd[2] };
	float direction[3] = { shotEnd[0] - from[0], shotEnd[1] - from[1], shotEnd[2] - from[2] };
	const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (length > 1.0f)
	{
		for (float &component : direction) component /= length;
		const float back = std::min(3.0f, length - 0.5f);
		const float start[3] = { shotEnd[0] - direction[0] * back, shotEnd[1] - direction[1] * back, shotEnd[2] - direction[2] * back };
		float surface[3];
		if (visibleSurfaceAlong(start, direction, back + 12.0f, surface))
		{
			for (int axis = 0; axis < 3; ++axis) head[axis] = surface[axis] - direction[axis] * 0.3f; // just in front of it
		}
	}
	const Uint8 debrisColor = debrisColorAt(head);
	const Uint8 sparkColors[3] = { _colors.spark, _colors.fire[1], _colors.fire[0] };
	const Uint8 chunkColors[2] = { debrisColor, shadeColor(debrisColor, 2) };
	const Uint8 dustColors[2] = { _colors.dust, _colors.smoke };
	// Sparks, flying fast and falling, gone within half a second (not from an object flying, an
	// arrow or a dart: it strikes no sparks). (The arguments of burst are explained at spawnDebris.)
	if (trail != TRAIL_FLYING)
	{
		_particles.burst(head, 14, P::GLOW, P::EFFECT, sparkColors, 3, 30, 90, 0.4f, 160, 1.0f, 0.2f, 0.5f, 2);
	}
	// Chunks of what was hit, thrown up and falling back within a second.
	_particles.burst(head, 8, P::LIT, P::EFFECT, chunkColors, 2, 20, 60, 0.6f, 220, 0.5f, 0.6f, 1.1f, 2);
	// A puff of dust, slowly rising, for a second or so.
	_particles.burst(head, 6, P::LIT, P::EFFECT, dustColors, 2, 3, 10, 0.5f, -6, 1.5f, 0.8f, 1.6f, 4);
	// The mark: a scorch (size 3, 6 shades darker) or a bullet hole (size 1, 4 shades darker).
	const bool scorch = trail == TRAIL_BEAM || trail == TRAIL_PLASMA || trail == TRAIL_FIRE;
	addDecal(head, scorch ? 3 : 1, scorch ? 6 : 4);
}

/**
 * Draws the head of a shot in flight and the streak it leaves behind it, as the original game
 * drew a round with a short trail: a dot and a spark-coloured streak for bullets, a glowing blob
 * for plasma, a flame for rockets and their exhaust; any shot with a trail glows (feeds the
 * bloom), and so do the streaks. An object flying (an arrow, a dart: TRAIL_FLYING) is only a
 * thin white streak, longer, with no dot. Both are as big as the shot's calibre (see
 * shotCalibre): a pistol's round the smallest, then a rifle's, an auto-cannon's, a heavy weapon's. The streak runs back along the
 * path, never further back than where the shot started, solid for its first half and fading for
 * the rest. The dot isn't drawn right in front of the eye (less than 6 voxels away), where it
 * would be a blob of huge pixels.
 * @param head Where the head is, in voxels.
 * @param origin Where the shot started (the muzzle for our own), in voxels.
 * @param trail How the shot looks in flight.
 * @param color The trail's colour.
 * @param calibre How big it is (1 a rifle's).
 */
void FirstPersonView::drawShotDot(const float head[3], const float origin[3], Trail trail, Uint8 color, float calibre)
{
	// Its colours: the dot's rim, middle and centre, and the streak's front and back halves.
	// Bullets their colour (spark-coloured, under the sea yellow); a rocket (or another explosive round) a flame, white-hot in yellow
	// in an orange rim, with a yellow-to-orange streak; incendiary rounds the same, burning
	// redder; the rest the trail's colour, white-hot in the middle (its colour group's brightest
	// shade).
	Uint8 rim = color, middle = color, centre = (Uint8)(color & 0xF0), streakFront = color, streakBack = shadeColor(color, 2);
	switch (trail)
	{
	case TRAIL_SMOKE: rim = _colors.fire[2]; middle = _colors.fire[1]; centre = _colors.fire[0]; streakFront = _colors.fire[1]; streakBack = _colors.fire[2]; break;
	case TRAIL_FIRE: rim = _colors.fire[2]; middle = _colors.fire[1]; centre = _colors.fire[0]; streakFront = _colors.fire[2]; streakBack = _colors.fire[3]; break;
	case TRAIL_FLYING: streakBack = shadeColor(color, 3); break;
	default: break;
	}

	// The streak: its length and width in the world (voxels) by the kind of shot and its
	// calibre, at most as long as the shot has flown; at most 1 + 1.5 x calibre pixels thick.
	const float back[3] = { origin[0] - head[0], origin[1] - head[1], origin[2] - head[2] };
	const float flown = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
	const float streakLength = std::min(flown, (trail == TRAIL_FLYING ? 14.0f : trail == TRAIL_NONE ? 10.0f : trail == TRAIL_SMOKE ? 9.0f : trail == TRAIL_PLASMA ? 7.0f : 6.0f) * calibre);
	if (streakLength > 1.0f)
	{
		float halfway[3], tail[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			halfway[axis] = head[axis] + back[axis] / flown * streakLength * 0.5f;
			tail[axis] = head[axis] + back[axis] / flown * streakLength;
		}
		const float streakWidth = (trail == TRAIL_PLASMA ? 1.6f : trail == TRAIL_SMOKE ? 1.0f : trail == TRAIL_FLYING ? 0.35f : 0.6f) * calibre;
		const int widest = 1 + (int)std::lround(calibre * 1.5f);
		drawLine3D(head, halfway, streakFront, widest, 1.0f, streakWidth);
		drawLine3D(halfway, tail, streakBack, widest, 0.6f, streakWidth * 0.7f);
	}

	// The dot: where the head is on the screen, its size in the world (voxels) by the kind of
	// shot and its calibre, and on the screen a radius in pixels, 1..12. (None for an object
	// flying: the streak is all of it.)
	if (trail == TRAIL_FLYING)
	{
		return;
	}
	const double world[3] = { head[0], head[1], head[2] };
	double screenX, screenY, depth;
	if (!projectToScreen(world, screenX, screenY, depth) || depth < 6.0)
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // in pixels
	const double size = (trail == TRAIL_PLASMA ? 3.5 : trail == TRAIL_SMOKE ? 1.1 : 0.9) * calibre;
	const int radius = std::clamp((int)std::lround(size * focalLength / depth * 0.5), 1, 12);
	// A round dot, depth-tested: the inner 40% in the centre colour, out to 70% the middle one,
	// the rest the rim.
	for (int y = (int)screenY - radius; y <= (int)screenY + radius; ++y)
	{
		for (int x = (int)screenX - radius; x <= (int)screenX + radius; ++x)
		{
			const double distance = std::sqrt((x - screenX) * (x - screenX) + (y - screenY) * (y - screenY));
			if (distance > radius || x < 0 || y < 0 || x >= width || y >= height || depth >= _depthBuffer[y * width + x]) continue;
			*getRaw(x, y) = distance < radius * 0.4 ? centre : distance < radius * 0.7 ? middle : rim;
			if (trail != TRAIL_NONE) _glowMask[y * width + x] = 1;
		}
	}
}

/**
 * Draws a laser beam: a coloured sheath round a white-hot core (its colour group's brightest
 * shade), each as thick as its width in the world looks where it is (thicker up close), the
 * widths by the calibre (see shotCalibre): a laser pistol's beam the thinnest, a heavy laser's
 * the widest; the sheath at most 2 (pistol) to 4 (heavy) pixels, the core 1 or 2.
 * @param from, to The beam's ends, in voxels.
 * @param color Its colour.
 * @param calibre How big the shot is (1 a rifle's).
 * @param opacity 1 solid .. 0 gone (a beam fading out).
 */
void FirstPersonView::drawBeam(const float from[3], const float to[3], Uint8 color, float calibre, float opacity)
{
	drawLine3D(from, to, color, (int)std::lround(1.5f + 1.5f * calibre), opacity, 0.3f * calibre);
	drawLine3D(from, to, (Uint8)(color & 0xF0), calibre > 1.25f ? 2 : 1, opacity, 0.12f * calibre);
}

/**
 * Dual wield: the other gun's round, fired with ours (Map::getPartnerProjectile). It is drawn
 * from the other gun's muzzle on a straight line to where it lands, with its trail; it kicks
 * the view and lights up the room too, and lands with sparks, a hole or a beam like any of our
 * shots. Called once per frame, from updateParticles.
 */
void FirstPersonView::spawnPartnerEffects()
{
	const Projectile *partner = _map ? _map->getPartnerProjectile() : nullptr;
	if (partner != _lastPartnerShot)
	{
		// The last round has landed: sparks and a mark where it ended, and a laser leaves its
		// beam fading out.
		if (_lastPartnerShot)
		{
			spawnShotImpact(_partnerHead, _partnerOrigin, _partnerTrail);
			if (_partnerTrail == TRAIL_BEAM)
			{
				_beams.push_back({ { _partnerOrigin[0], _partnerOrigin[1], _partnerOrigin[2] },
					{ _partnerHead[0], _partnerHead[1], _partnerHead[2] }, SDL_GetTicks(), _partnerColor, _partnerCalibre });
			}
		}
		if (partner)
		{
			// A new round: the game's path, from its first voxel to its last (all in voxels).
			const Position firedFrom = partner->getPosition(-100000), landsAt = partner->getPosition(100000);
			float origin[3] = { (float)firedFrom.x, (float)firedFrom.y, (float)firedFrom.z };
			std::copy(origin, origin + 3, _partnerFrom);
			_partnerLength = std::max(1.0f, (float)std::sqrt(Position::distanceSq(firedFrom, landsAt)));
			// Drawn from the other gun's muzzle on the screen, 12 voxels out along the ray
			// through it: the offset from the game's start, which shrinks to nothing at the
			// impact (see the head below).
			std::fill(_partnerOffset, _partnerOffset + 3, 0.0f);
			if (_offHandMuzzleValid)
			{
				double rayDirection[3];
				getRayDirection((int)_offHandMuzzleScreen[0], (int)_offHandMuzzleScreen[1], rayDirection);
				for (int axis = 0; axis < 3; ++axis)
				{
					origin[axis] = (float)(_eye[axis] + rayDirection[axis] * 12.0);
					_partnerOffset[axis] = origin[axis] - _partnerFrom[axis];
				}
			}
			std::copy(origin, origin + 3, _partnerOrigin);
			std::copy(origin, origin + 3, _partnerHead);
			// Its look in flight, when it left (the weapon in hand flashes), the muzzle flash
			// lighting up the room, and the kick: by the ammo's power, 0.5 to 2.5 degrees,
			// adding up to at most 6 (not in a helmet camera's view: the round is the soldier
			// we look through's, see beginShot).
			trailFor(partner->getAmmo(), partner->getWeapon(), _partnerTrail, _partnerColor, _partnerCalibre);
			_partnerShotTick = SDL_GetTicks();
			_partnerShotAmmo = partner->getAmmo() ? partner->getAmmo()->getRules() : nullptr;
			const float power = partner->getAmmo() ? (float)partner->getAmmo()->getRules()->getPower() : 30.0f;
			const bool flying = flyingShot(_partnerShotAmmo, partner->getWeapon() ? partner->getWeapon()->getRules() : nullptr);
			if (!flying) // (an object flying has no flash, and barely kicks)
			{
				std::copy(origin, origin + 3, _muzzleFlashPos); // the flash lights up the room
				_muzzleFlashTick = _partnerShotTick;
			}
			if (_particlesOn)
			{
				_recoilKick = std::min(_recoilKick + std::clamp(power / 30.0f, 0.5f, 2.5f) * (flying ? 0.3f : 1.0f), 6.0f);
			}
		}
		_lastPartnerShot = partner;
	}
	if (partner)
	{
		// The round flies on: its head is the game's position plus the share of the muzzle
		// offset still left (1 at the start .. 0 at the end of the path), so it runs on a
		// straight line from the muzzle to the impact.
		const float previous[3] = { _partnerHead[0], _partnerHead[1], _partnerHead[2] };
		const Position gamePosition = partner->getPosition(0);
		const float gameHead[3] = { (float)gamePosition.x, (float)gamePosition.y, (float)gamePosition.z };
		float travelledSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			travelledSquared += (gameHead[axis] - _partnerFrom[axis]) * (gameHead[axis] - _partnerFrom[axis]);
		}
		const float remaining = std::max(0.0f, 1.0f - std::sqrt(travelledSquared) / _partnerLength);
		for (int axis = 0; axis < 3; ++axis) _partnerHead[axis] = gameHead[axis] + _partnerOffset[axis] * remaining;
		// Its trail: spawnTrailSegment draws the main shot's look, so swap this round's in for
		// the call and back.
		std::swap(_shotTrail, _partnerTrail);
		std::swap(_shotTrailColor, _partnerColor);
		std::swap(_shotCalibre, _partnerCalibre);
		spawnTrailSegment(previous, _partnerHead);
		std::swap(_shotTrail, _partnerTrail);
		std::swap(_shotTrailColor, _partnerColor);
		std::swap(_shotCalibre, _partnerCalibre);
	}
}

/**
 * Where the shot in flight is drawn: the game's position, and for our own shot that position
 * plus the share of the muzzle offset still left (all of it at the start, none at the end of
 * the path), so it leaves the muzzle of the weapon in hand and lands where the game says.
 * @param projectile The game's projectile in flight.
 * @param out Gets where its head is drawn, in voxels.
 */
void FirstPersonView::shotHead(const Projectile *projectile, float out[3]) const
{
	const Position gamePosition = projectile->getPosition(0);
	const float gameHead[3] = { (float)gamePosition.x, (float)gamePosition.y, (float)gamePosition.z };
	float travelledSquared = 0.0f;
	for (int axis = 0; axis < 3; ++axis) travelledSquared += (gameHead[axis] - _shotFrom[axis]) * (gameHead[axis] - _shotFrom[axis]);
	// The game's path is a straight line, so this draws a straight line from the muzzle to the hit.
	const float remaining = std::max(0.0f, 1.0f - std::sqrt(travelledSquared) / _shotLength);
	for (int axis = 0; axis < 3; ++axis) out[axis] = gameHead[axis] + _shotOffset[axis] * remaining;
}

/**
 * The path a throw or shot aimed now would take (setting "Show trajectory", or a target marked
 * for confirm fire; see FirstPersonMode::updateTrajectory): dashes of TrajectoryDash voxels
 * along it in a light cyan (no other line is that colour), two pixels thick, fading a little
 * towards the far end, so the eye follows it out; and where it ends (where a throw lands, or a
 * shot meets something), a diamond lying flat round it. The first voxels, at the soldier's own hand, are left
 * out. Depth-tested like any line, so walls hide what's behind them.
 */
void FirstPersonView::drawTrajectory()
{
	if (_trajectory.size() < 2)
	{
		return;
	}
	const float TrajectoryDash = 6.0f, TrajectorySkip = 14.0f;

	// The path's length, to fade the dashes along it.
	auto voxelCentre = [](const Position &voxel, float out[3]) { out[0] = voxel.x + 0.5f; out[1] = voxel.y + 0.5f; out[2] = voxel.z + 0.5f; };
	float totalLength = 0.0f;
	for (size_t point = 1; point < _trajectory.size(); ++point)
	{
		const Position step = _trajectory[point] - _trajectory[point - 1];
		totalLength += std::sqrt((float)(step.x * step.x + step.y * step.y + step.z * step.z));
	}
	if (totalLength <= TrajectorySkip)
	{
		return;
	}

	// Every other stretch of TrajectoryDash voxels is a dash: drawn as one line from where it
	// starts to where it ends.
	const Uint8 color = _colors.trajectory;
	float travelled = 0.0f, dashStart[3] = {}, previous[3];
	bool inDash = false;
	voxelCentre(_trajectory[0], previous);
	for (size_t point = 1; point < _trajectory.size(); ++point)
	{
		float current[3];
		voxelCentre(_trajectory[point], current);
		const float stepLength = std::sqrt((current[0] - previous[0]) * (current[0] - previous[0]) + (current[1] - previous[1]) * (current[1] - previous[1])
			+ (current[2] - previous[2]) * (current[2] - previous[2]));
		travelled += stepLength;
		const bool dashHere = travelled > TrajectorySkip && ((int)(travelled / TrajectoryDash) % 2) == 0;
		if (dashHere && !inDash)
		{
			std::copy(previous, previous + 3, dashStart);
		}
		if (inDash && (!dashHere || point + 1 == _trajectory.size()))
		{
			const float opacity = 1.0f - 0.4f * std::min(1.0f, travelled / totalLength);
			drawLine3D(dashStart, dashHere ? current : previous, color, 2, opacity, 0.0f, 0, 1 << 30, Options::fpsGlowingOutlines);
		}
		inDash = dashHere;
		std::copy(current, current + 3, previous);
	}

	// The end: a diamond, flat, round the last voxel.
	float end[3];
	voxelCentre(_trajectory.back(), end);
	const float corners[4][3] = { { end[0] + 6.0f, end[1], end[2] }, { end[0], end[1] + 6.0f, end[2] }, { end[0] - 6.0f, end[1], end[2] }, { end[0], end[1] - 6.0f, end[2] } };
	for (int side = 0; side < 4; ++side)
	{
		drawLine3D(corners[side], corners[(side + 1) % 4], color, 2, 1.0f, 0.0f, 0, 1 << 30, Options::fpsGlowingOutlines);
	}
}

/**
 * Laser (and gauss) beams: a line from the muzzle to wherever the shot is (or ended), a bright
 * core in a coloured sheath as wide as the shot's calibre (see drawBeam), fading out over a third
 * of a second (350 ms) after the hit.
 */
void FirstPersonView::drawBeams()
{
	// The beams of shots that have ended, fading out; forget those faded away.
	const Uint32 now = SDL_GetTicks();
	_beams.erase(std::remove_if(_beams.begin(), _beams.end(), [now](const Beam &beam) { return now - beam.start > 350; }), _beams.end());
	for (const auto &beam : _beams)
	{
		const float fade = 1.0f - (now - beam.start) / 350.0f; // opacity: 1 at the hit .. 0 gone
		drawBeam(beam.from, beam.to, beam.color, beam.calibre, fade);
	}
	// The beam of the shot in flight (not a thrown item), solid, from the muzzle to its head.
	Projectile *projectile = _map ? _map->getProjectile() : nullptr;
	if (projectile && projectile == _lastProjectile && !_lastProjectileThrown && _shotTrail == TRAIL_BEAM)
	{
		drawBeam(_shotTrailOrigin, _lastShotHead, _shotTrailColor, _shotCalibre, 1.0f);
	}
	// And dual wield's second round's, the same way.
	if (_map && _map->getPartnerProjectile() && _partnerTrail == TRAIL_BEAM)
	{
		drawBeam(_partnerOrigin, _partnerHead, _partnerColor, _partnerCalibre, 1.0f);
	}
}

/**
 * Follows the shot in flight (the game has one at a time), once per frame: when it changes,
 * the last one has ended (finishShot) and a new one may have begun (beginShot); while it
 * flies, its trail grows (advanceShot).
 */
void FirstPersonView::followShots()
{
	Projectile *projectile = _map ? _map->getProjectile() : nullptr;
	if (projectile != _lastProjectile)
	{
		finishShot();
		if (projectile)
		{
			beginShot(projectile);
		}
		_lastProjectile = projectile;
	}
	if (projectile)
	{
		advanceShot(projectile);
	}
}

/**
 * The last shot has ended (if there was one): where it landed, sparks and a mark (not for a
 * thrown item); a laser leaves its beam fading out; an enemy shot notes when it ended (a hit
 * on our soldier right now came from it).
 */
void FirstPersonView::finishShot()
{
	if (_lastProjectile && !_lastProjectileThrown)
	{
		spawnShotImpact(_lastShotHead, _shotTrailOrigin, _shotTrail);
	}
	if (_lastProjectile && _shotIsEnemy)
	{
		_enemyShotEnd = SDL_GetTicks(); // (a hit on our soldier right now came from _enemyShotFrom)
	}
	if (_lastProjectile && !_lastProjectileThrown && _shotTrail == TRAIL_BEAM)
	{
		_beams.push_back({ { _shotTrailOrigin[0], _shotTrailOrigin[1], _shotTrailOrigin[2] },
			{ _lastShotHead[0], _lastShotHead[1], _lastShotHead[2] }, SDL_GetTicks(), _shotTrailColor, _shotCalibre });
	}
}

/**
 * A new shot has left: where it starts (our own from the muzzle of the weapon in hand), whose
 * it is (ours, an enemy's, or a thrown item), how it looks in flight, and the muzzle flash
 * that lights the room for a moment; our own shot kicks the view up.
 * @param projectile The game's new projectile.
 */
void FirstPersonView::beginShot(Projectile *projectile)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// Where the game's path starts and where the shot is now (in voxels): the flash at the
	// muzzle points from one to the other.
	const Position firedFrom = projectile->getPosition(-100000), current = projectile->getPosition(0);
	float origin[3] = { (float)firedFrom.x, (float)firedFrom.y, (float)firedFrom.z };
	_lastProjectileThrown = projectile->getItem() != nullptr;
	if (_lastProjectileThrown)
	{
		_thrownStart = SDL_GetTicks(); // it tumbles from here
	}
	// Whose shot: one starting within 20 voxels of the eye is our own (and the weapon in hand
	// flashes); any other shot (not a thrown item; a squadmate's counts too) is taken for an
	// enemy's, which may whizz past us and whose origin a hit on us points back to. A helmet
	// camera's view (the one without particles of its own, see setParticles) has no shots of
	// its own: no weapon in hand, no kick.
	const double dx = origin[0] - _eye[0], dy = origin[1] - _eye[1], dz = origin[2] - _eye[2]; // from the eye
	const bool ownShot = _particlesOn && !_lastProjectileThrown && dx * dx + dy * dy + dz * dz < 20.0 * 20.0;
	if (ownShot)
	{
		_ownShotTick = SDL_GetTicks();
	}
	_shotIsEnemy = !ownShot && !_lastProjectileThrown;
	_nearMissDone = !_shotIsEnemy;
	if (_shotIsEnemy)
	{
		for (int axis = 0; axis < 3; ++axis) _enemyShotFrom[axis] = origin[axis];
	}
	// The game's path, from its first voxel to its last.
	const Position landsAt = projectile->getPosition(100000);
	for (int axis = 0; axis < 3; ++axis) _shotFrom[axis] = origin[axis];
	_shotLength = std::max(1.0f, (float)std::sqrt(Position::distanceSq(firedFrom, landsAt)));
	// Our own shot leaves the weapon in hand: from its muzzle on the screen, 12 voxels out
	// along the ray through it. The offset from the game's start shrinks to nothing at the
	// impact (see shotHead).
	std::fill(_shotOffset, _shotOffset + 3, 0.0f);
	if (ownShot && _muzzleValid)
	{
		double rayDirection[3];
		getRayDirection((int)_muzzleScreen[0], (int)_muzzleScreen[1], rayDirection);
		for (int axis = 0; axis < 3; ++axis)
		{
			origin[axis] = (float)(_eye[axis] + rayDirection[axis] * 12.0);
			_shotOffset[axis] = origin[axis] - _shotFrom[axis];
		}
	}
	std::copy(origin, origin + 3, _shotTrailOrigin);
	_lastShotHead[0] = origin[0]; _lastShotHead[1] = origin[1]; _lastShotHead[2] = origin[2];
	// What the shot looks like in flight, from its damage type (a thrown item: nothing).
	const BattleItem *ammo = projectile->getAmmo();
	if (ownShot)
	{
		_ownShotAmmo = ammo ? ammo->getRules() : nullptr;
	}
	_shotTrail = TRAIL_NONE;
	_shotTrailColor = _colors.spark;
	_shotCalibre = 1.0f;
	if (!_lastProjectileThrown)
	{
		trailFor(ammo, projectile->getWeapon(), _shotTrail, _shotTrailColor, _shotCalibre);
	}
	if (!_lastProjectileThrown)
	{
		// The flash lights up the room for a moment (an object flying, such as an arrow, has none).
		const bool flying = flyingShot(ammo ? ammo->getRules() : nullptr, projectile->getWeapon() ? projectile->getWeapon()->getRules() : nullptr);
		if (!flying)
		{
			std::copy(origin, origin + 3, _muzzleFlashPos);
			_muzzleFlashTick = SDL_GetTicks();
		}
		// Our own shot kicks the view: hard for guns, by their power (0.8 to 3.5 degrees);
		// plasma less, lasers and objects flying (a bow's arrow) barely; adding up to at most 6 degrees. The camera shakes a
		// little too.
		if (ownShot)
		{
			const float power = ammo ? (float)ammo->getRules()->getPower() : 30.0f;
			const float weight = _shotTrail == TRAIL_BEAM ? 0.35f : flying ? 0.3f : _shotTrail == TRAIL_PLASMA ? 0.7f : 1.0f;
			_recoilKick = std::min(_recoilKick + std::clamp(power / 22.0f, 0.8f, 3.5f) * weight, 6.0f);
			addShake(0.25f + 0.1f * weight);
		}
		// The direction the shot leaves in (from the start of its path to where it is now).
		float direction[3] = { (float)(current.x - firedFrom.x), (float)(current.y - firedFrom.y), (float)(current.z - firedFrom.z) };
		const float length = std::max(1.0f,
			std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]));
		// At the muzzle (if the muzzle flash option is on): a quick bright flash of a dozen
		// sparks shooting off towards the target, and a little smoke. (Our own shot's flash is
		// drawn on the weapon in hand; sparks and smoke right before the eyes would be a few
		// huge squares.)
		const bool atMuzzle = ownShot && _muzzleValid;
		for (int i = 0; i < (Options::fpsMuzzleFlash && !atMuzzle ? 12 : 0); ++i)
		{
			P::Particle spark;
			const float speed = _particles.random(20, 70); // voxels per second
			for (int axis = 0; axis < 3; ++axis)
			{
				spark.pos[axis] = origin[axis];
				spark.vel[axis] = direction[axis] / length * speed + _particles.random(-12, 12);
			}
			spark.age = 0; spark.life = _particles.random(0.05f, 0.12f); spark.gravity = 0; spark.drag = 4;
			spark.color = _colors.fire[(int)_particles.random(0, 2)]; spark.size = 3; spark.kind = P::GLOW; spark.group = P::EFFECT;
			_particles.add(spark);
		}
		// The smoke: four slow puffs, rising, for about a second. (The arguments of burst are
		// explained at spawnDebris.)
		const Uint8 smokeColors[1] = { _colors.smoke };
		if (!atMuzzle) _particles.burst(origin, 4, P::LIT, P::EFFECT, smokeColors, 1, 2, 8, 0.6f, -8, 1.2f, 0.8f, 1.4f, 3);
	}
}

/**
 * The shot moved on, once per frame: its trail grows to where it is now, and an enemy shot
 * passing close by the eye (within 14 voxels) whizzes past (once per shot): the time and the
 * side it passed on are noted for the first-person mode, which plays the whizz on that side
 * and jolts the view (FirstPersonMode::checkNearMiss).
 * @param projectile The game's projectile in flight.
 */
void FirstPersonView::advanceShot(Projectile *projectile)
{
	const float previous[3] = { _lastShotHead[0], _lastShotHead[1], _lastShotHead[2] };
	shotHead(projectile, _lastShotHead);
	spawnTrailSegment(previous, _lastShotHead);
	if (!_nearMissDone)
	{
		// An enemy shot passing close by the eye whizzes past (once per shot). The point of
		// this frame's bit of path nearest the eye: the eye projected onto it, kept within it.
		float segment[3], toEye[3], lengthSquared = 0.0f, eyeAlong = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			segment[axis] = _lastShotHead[axis] - previous[axis];
			toEye[axis] = (float)_eye[axis] - previous[axis];
			lengthSquared += segment[axis] * segment[axis];
			eyeAlong += segment[axis] * toEye[axis];
		}
		// How far along the bit of path that point is: 0 at the last head .. 1 at this one.
		const float progress = lengthSquared > 0.0f ? std::clamp(eyeAlong / lengthSquared, 0.0f, 1.0f) : 0.0f;
		float distanceSquared = 0.0f, nearest[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			nearest[axis] = previous[axis] + segment[axis] * progress;
			distanceSquared += (nearest[axis] - (float)_eye[axis]) * (nearest[axis] - (float)_eye[axis]);
		}
		// Within 14 voxels: note when, and the direction from the eye to where it passed
		// (across the map).
		if (distanceSquared < 14.0f * 14.0f)
		{
			_nearMissDone = true;
			_nearMissTick = SDL_GetTicks();
			_nearMissDirection[0] = nearest[0] - _eye[0];
			_nearMissDirection[1] = nearest[1] - _eye[1];
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
