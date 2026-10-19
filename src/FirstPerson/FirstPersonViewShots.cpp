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

namespace OpenXcom
{

/**
 * Draws what flies and what explodes, as sprites and dots in the world: a thrown item (its own
 * sprite, at the game's position), the head of a shot in flight (a dot, blob or flame; beams
 * are drawn as lines by drawBeams), dual wield's second round, and the explosions with the
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
	// The game's one projectile in flight.
	if (Projectile *projectile = _map->getProjectile())
	{
		if (BattleItem *item = projectile->getItem())
		{
			// A thrown item: its sprite as the map draws it, put into the sprite buffer at
			// (16, 8) and stood at the item's position in the world.
			const Position position = projectile->getPosition();
			const double world[3] = { (double)position.x, (double)position.y, (double)position.z };
			ItemSprite itemSprite(_spriteBuffer, mod, _save, 0);
			_spriteBuffer->clear();
			itemSprite.draw(item, 16, 8, 0);
			drawSpriteSmooth(_spriteBuffer, 32, 34, world, 4.0, 0); // like Map: the voxel sits at sprite pixel (16, 26)
		}
		else if (_shotTrail != TRAIL_BEAM) // beams are drawn as lines
		{
			// The head of the shot: a small dot for bullets, a glowing blob for plasma, a flame for rockets.
			float head[3];
			shotHead(projectile, head);
			drawShotDot(head, _shotTrail, _shotTrailColor);
		}
	}
	if (_map->getPartnerProjectile() && _partnerTrail != TRAIL_BEAM)
	{
		drawShotDot(_partnerHead, _partnerTrail, _partnerColor); // dual wield: the other gun's round
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
		// The game's sprite for this frame and the next (none after the last).
		const Position position = explosion->getPosition();
		const double world[3] = { (double)position.x, (double)position.y, (double)position.z };
		SurfaceSet *spriteSet = mod->getSurfaceSet(explosion->isBig() ? "X1.PCK" : explosion->isHit() ? "HIT.PCK" : "SMOKE.PCK");
		const Surface *sprite = spriteSet->getFrame(frame);
		if (!sprite)
		{
			continue;
		}
		const Surface *nextSprite = frame + 1 < (int)spriteSet->getTotalFrames() ? spriteSet->getFrame(frame + 1) : nullptr;
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
 * embers for incendiary rounds, smoke for rockets and other explosives, green glow for plasma,
 * sparkles for stun, droplets for acid. Under the sea every shot (and thrown thing) also leaves
 * bubbles that hang in the water for half a minute. The look is _shotTrail and _shotTrailColor
 * (spawnPartnerEffects swaps dual wield's second round's in for its own trail).
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
	// One particle for about every 3 voxels flown, each at a random spot within its share of
	// the path.
	const int steps = std::max(1, (int)(length / 3.0f));
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
			// Smoke: puffs in two greys, slowing down and hanging for a couple of seconds.
			particle.vel[0] = _particles.random(-2, 2); particle.vel[1] = _particles.random(-2, 2);
			particle.vel[2] = _particles.random(1, 4);
			particle.life = _particles.random(1.5f, 3.0f);
			particle.color = _particles.random() < 0.5f ? _colors.smoke : shadeColor(_colors.smoke, 2);
			particle.size = 4; particle.kind = P::LIT;
			particle.drag = 0.8f;
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
 * Draws a line in the world (a laser beam), depth-tested, a pixel or two thick, faded by
 * dithering; it glows (its pixels feed the bloom).
 * @param from, to The line's ends, in voxels.
 * @param color Its palette colour.
 * @param thickness How thick it is on the screen, in pixels.
 * @param opacity 1 solid .. 0 gone (dithered): a beam fading out.
 */
void FirstPersonView::drawLine3D(const float from[3], const float to[3], Uint8 color, int thickness, float opacity)
{
	const int width = getWidth(), height = getHeight();
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
		// A square of thickness x thickness pixels around the point; in front of the scene
		// (with a voxel to spare, so a beam grazing a wall still shows), dithered by opacity.
		for (int offsetY = 0; offsetY < thickness; ++offsetY)
		{
			for (int offsetX = 0; offsetX < thickness; ++offsetX)
			{
				const int pixelX = x + offsetX - thickness / 2, pixelY = y + offsetY - thickness / 2;
				if (pixelX < 0 || pixelY < 0 || pixelX >= width || pixelY >= height
					|| depth >= _depthBuffer[pixelY * width + pixelX] + 1.0f) continue;
				if (opacity < 1.0f && dither(pixelX, pixelY) >= opacity) continue;
				*getRaw(pixelX, pixelY) = color;
				_glowMask[pixelY * width + pixelX] = 1;
			}
		}
	}
}

/**
 * How a shot looks in flight, from its ammo's damage type: the kind of trail and its colour.
 * Incendiary: embers; high explosive: smoke; laser: a red beam; plasma: a green blob; stun:
 * pale blue sparkles; acid: yellow-green droplets; anything else (bullets and the like): no
 * trail, a spark-coloured dot. Under the sea lasers are a pale blue and plasma a pale cyan.
 * The colours are the nearest the current palette has.
 * @param ammo The ammo fired (null if unknown).
 * @param trail Gets the kind of trail.
 * @param color Gets its palette colour.
 */
void FirstPersonView::trailFor(const BattleItem *ammo, Trail &trail, Uint8 &color) const
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
	switch (damage ? damage->ResistType : DT_NONE)
	{
	case DT_IN: trail = TRAIL_FIRE; color = _colors.fire[1]; break;
	case DT_HE: trail = TRAIL_SMOKE; color = _colors.smoke; break;
	case DT_LASER: trail = TRAIL_BEAM; color = _colors.underwater ? nearest(90, 220, 255) : nearest(255, 40, 30); break;
	case DT_PLASMA: trail = TRAIL_PLASMA; color = _colors.underwater ? nearest(180, 240, 255) : nearest(90, 255, 90); break;
	case DT_STUN: trail = TRAIL_STUN; color = nearest(160, 190, 255); break;
	case DT_ACID: trail = TRAIL_ACID; color = nearest(190, 255, 60); break;
	default: trail = TRAIL_NONE; color = _colors.spark; break;
	}
}

/**
 * Where a shot ended, if not in a unit (that bleeds or sparks, see watchUnits): sparks, chunks
 * in the colour of what was hit and a puff of dust, and a mark: a bullet hole, or a bigger
 * scorch from beams, plasma and incendiaries.
 * @param head Where the shot ended, in voxels.
 * @param trail How it looked in flight (from its damage type).
 */
void FirstPersonView::spawnShotImpact(const float head[3], Trail trail)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// Nothing here if there is a unit in the tile it ended in, or in the tile below (a tall
	// unit reaches up into the next level).
	const Position headTile((int)head[0] / 16, (int)head[1] / 16, (int)head[2] / 24);
	const Tile *tile = _save->getTile(headTile);
	if (tile && (tile->getUnit() || (headTile.z > 0 && _save->getTile(headTile - Position(0, 0, 1))->getUnit())))
	{
		return;
	}
	const Uint8 debrisColor = debrisColorAt(head);
	const Uint8 sparkColors[3] = { _colors.spark, _colors.fire[1], _colors.fire[0] };
	const Uint8 chunkColors[2] = { debrisColor, shadeColor(debrisColor, 2) };
	const Uint8 dustColors[2] = { _colors.dust, _colors.smoke };
	// Sparks, flying fast and falling, gone within half a second. (The arguments of burst are
	// explained at spawnDebris.)
	_particles.burst(head, 14, P::GLOW, P::EFFECT, sparkColors, 3, 30, 90, 0.4f, 160, 1.0f, 0.2f, 0.5f, 2);
	// Chunks of what was hit, thrown up and falling back within a second.
	_particles.burst(head, 8, P::LIT, P::EFFECT, chunkColors, 2, 20, 60, 0.6f, 220, 0.5f, 0.6f, 1.1f, 2);
	// A puff of dust, slowly rising, for a second or so.
	_particles.burst(head, 6, P::LIT, P::EFFECT, dustColors, 2, 3, 10, 0.5f, -6, 1.5f, 0.8f, 1.6f, 4);
	// The mark: a scorch (size 3, 6 shades darker) or a bullet hole (size 1, 4 shades darker).
	const bool scorch = trail == TRAIL_BEAM || trail == TRAIL_PLASMA || trail == TRAIL_FIRE;
	addDecal(head, scorch ? 3 : 1, scorch ? 6 : 4);
}

/**
 * Draws the head of a shot in flight: a small dot for bullets, a glowing blob for plasma, a
 * flame for rockets; any shot with a trail glows (feeds the bloom). Nothing is drawn right in
 * front of the eye (less than 6 voxels away), where it would be a blob of huge pixels.
 * @param head Where the head is, in voxels.
 * @param trail How the shot looks in flight.
 * @param color The trail's colour.
 */
void FirstPersonView::drawShotDot(const float head[3], Trail trail, Uint8 color)
{
	const double world[3] = { head[0], head[1], head[2] };
	double screenX, screenY, depth;
	if (!projectToScreen(world, screenX, screenY, depth) || depth < 6.0)
	{
		return;
	}
	// Its size in the world (voxels) by the kind of shot, and on the screen (a radius in
	// pixels, 1..12); its colour (bullets spark-coloured, rockets fire-coloured).
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // in pixels
	const double size = trail == TRAIL_PLASMA ? 5.0 : trail == TRAIL_SMOKE ? 3.0 : 1.2;
	const int radius = std::clamp((int)std::lround(size * focalLength / depth * 0.5), 1, 12);
	const Uint8 coreColor = trail == TRAIL_NONE ? _colors.spark : trail == TRAIL_SMOKE ? _colors.fire[0] : color;
	// A round dot, depth-tested, with a white-hot centre (its colour group's brightest shade).
	for (int y = (int)screenY - radius; y <= (int)screenY + radius; ++y)
	{
		for (int x = (int)screenX - radius; x <= (int)screenX + radius; ++x)
		{
			const double distance = std::sqrt((x - screenX) * (x - screenX) + (y - screenY) * (y - screenY));
			if (distance > radius || x < 0 || y < 0 || x >= width || y >= height || depth >= _depthBuffer[y * width + x]) continue;
			*getRaw(x, y) = distance < radius * 0.5 ? (Uint8)(coreColor & 0xF0) : coreColor; // white-hot centre
			if (trail != TRAIL_NONE) _glowMask[y * width + x] = 1;
		}
	}
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
			spawnShotImpact(_partnerHead, _partnerTrail);
			if (_partnerTrail == TRAIL_BEAM)
			{
				_beams.push_back({ { _partnerOrigin[0], _partnerOrigin[1], _partnerOrigin[2] },
					{ _partnerHead[0], _partnerHead[1], _partnerHead[2] }, SDL_GetTicks(), _partnerColor });
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
			// adding up to at most 6.
			trailFor(partner->getAmmo(), _partnerTrail, _partnerColor);
			_partnerShotTick = SDL_GetTicks();
			_partnerShotAmmo = partner->getAmmo() ? partner->getAmmo()->getRules() : nullptr;
			std::copy(origin, origin + 3, _muzzleFlashPos); // the flash lights up the room
			_muzzleFlashTick = _partnerShotTick;
			const float power = partner->getAmmo() ? (float)partner->getAmmo()->getRules()->getPower() : 30.0f;
			_recoilKick = std::min(_recoilKick + std::clamp(power / 30.0f, 0.5f, 2.5f), 6.0f);
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
		spawnTrailSegment(previous, _partnerHead);
		std::swap(_shotTrail, _partnerTrail);
		std::swap(_shotTrailColor, _partnerColor);
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
 * Laser (and gauss) beams: a line from the muzzle to wherever the shot is (or ended), a bright
 * core in a coloured sheath, fading out over a third of a second (350 ms) after the hit.
 */
void FirstPersonView::drawBeams()
{
	// The beams of shots that have ended, fading out; forget those faded away.
	const Uint32 now = SDL_GetTicks();
	_beams.erase(std::remove_if(_beams.begin(), _beams.end(), [now](const Beam &beam) { return now - beam.start > 350; }), _beams.end());
	for (const auto &beam : _beams)
	{
		const float fade = 1.0f - (now - beam.start) / 350.0f; // opacity: 1 at the hit .. 0 gone
		drawLine3D(beam.from, beam.to, beam.color, 2, fade);
		drawLine3D(beam.from, beam.to, shadeColor(beam.color, 0) & 0xF0, 1, fade); // the white-hot core: the group's brightest shade
	}
	// The beam of the shot in flight (not a thrown item), solid, from the muzzle to its head.
	Projectile *projectile = _map ? _map->getProjectile() : nullptr;
	if (projectile && projectile == _lastProjectile && !_lastProjectileThrown && _shotTrail == TRAIL_BEAM)
	{
		drawLine3D(_shotTrailOrigin, _lastShotHead, _shotTrailColor, 2, 1.0f);
		drawLine3D(_shotTrailOrigin, _lastShotHead, (Uint8)(_shotTrailColor & 0xF0), 1, 1.0f);
	}
	// And dual wield's second round's, the same way.
	if (_map && _map->getPartnerProjectile() && _partnerTrail == TRAIL_BEAM)
	{
		drawLine3D(_partnerOrigin, _partnerHead, _partnerColor, 2, 1.0f);
		drawLine3D(_partnerOrigin, _partnerHead, (Uint8)(_partnerColor & 0xF0), 1, 1.0f);
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
		spawnShotImpact(_lastShotHead, _shotTrail);
	}
	if (_lastProjectile && _shotIsEnemy)
	{
		_enemyShotEnd = SDL_GetTicks(); // (a hit on our soldier right now came from _enemyShotFrom)
	}
	if (_lastProjectile && !_lastProjectileThrown && _shotTrail == TRAIL_BEAM)
	{
		_beams.push_back({ { _shotTrailOrigin[0], _shotTrailOrigin[1], _shotTrailOrigin[2] },
			{ _lastShotHead[0], _lastShotHead[1], _lastShotHead[2] }, SDL_GetTicks(), _shotTrailColor });
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
	// Whose shot: one starting within 20 voxels of the eye is our own (and the weapon in hand
	// flashes); any other shot (not a thrown item; a squadmate's counts too) is taken for an
	// enemy's, which may whizz past us and whose origin a hit on us points back to.
	const double dx = origin[0] - _eye[0], dy = origin[1] - _eye[1], dz = origin[2] - _eye[2]; // from the eye
	const bool ownShot = !_lastProjectileThrown && dx * dx + dy * dy + dz * dz < 20.0 * 20.0;
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
	if (!_lastProjectileThrown)
	{
		trailFor(ammo, _shotTrail, _shotTrailColor);
	}
	if (!_lastProjectileThrown)
	{
		// The flash lights up the room for a moment.
		std::copy(origin, origin + 3, _muzzleFlashPos);
		_muzzleFlashTick = SDL_GetTicks();
		// Our own shot kicks the view: hard for guns, by their power (0.8 to 3.5 degrees);
		// plasma less, lasers barely; adding up to at most 6 degrees. The camera shakes a
		// little too.
		if (ownShot)
		{
			const float power = ammo ? (float)ammo->getRules()->getPower() : 30.0f;
			const float weight = _shotTrail == TRAIL_BEAM ? 0.35f : _shotTrail == TRAIL_PLASMA ? 0.7f : 1.0f;
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
