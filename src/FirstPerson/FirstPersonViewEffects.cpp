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
 * FirstPersonViewEffects.cpp: particles in the world.
 *
 * Sparks, smoke and fire from explosions and burning tiles and units; the weather (rain, snow,
 * blowing sand, fireflies, dust motes); bubbles and marine snow under the sea; energy around UFO
 * power sources and blinking consoles; and the glow (bloom) around bright things.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <set>
#include <string>
#include <SDL.h>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Battlescape/Explosion.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Position.h"
#include "FirstPersonParticles.h"
#include "RuleFirstPerson.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * Tells whether there is open sky above a point: no floor and no object on any level above
 * the one the point is on. Rain, snow, blowing sand and fireflies only appear under open sky,
 * so nothing falls indoors or under a roof. A point outside the map always counts as under
 * open sky (the ground outside the map runs on to the horizon with nothing over it).
 * @param x, y, z The point, in voxels.
 * @return True if nothing on the map covers the point.
 */
bool FirstPersonView::openSky(float x, float y, float z) const
{
	// The map's size in tiles, and the column of tiles the point is in (a tile is 16 voxels
	// across).
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	const int tileX = (int)std::floor(x / 16), tileY = (int)std::floor(y / 16);
	if (tileX < 0 || tileY < 0 || tileX >= mapSizeX || tileY >= mapSizeY)
	{
		return true;
	}
	// Look at every level above the point's own (a level is 24 voxels high): a floor or an
	// object up there is a roof over the point.
	for (int tileZ = std::max(0, (int)std::floor(z / 24) + 1); tileZ < mapSizeZ; ++tileZ)
	{
		const TileView &tileView = _tileViews[(tileZ * mapSizeY + tileY) * mapSizeX + tileX];
		if (tileView.part[O_FLOOR] || tileView.part[O_OBJECT])
		{
			return false;
		}
	}
	return true;
}

/**
 * Under the sea: tells whether a point is in open water, where bubbles and marine snow may
 * float. It is not when the point is inside something solid, or when there is a floor on the
 * level right above it (the point is inside the sub or a building). Ruins towering further up
 * don't matter. A point outside the map counts as open water.
 * @param x, y, z The point, in voxels.
 * @return True if the point is in open water.
 */
bool FirstPersonView::inWater(float x, float y, float z) const
{
	// The map's size in tiles, and the tile the point is in (16 voxels across, 24 high).
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	const int tileX = (int)std::floor(x / 16), tileY = (int)std::floor(y / 16), tileZ = (int)std::floor(z / 24);
	if (tileX < 0 || tileY < 0 || tileX >= mapSizeX || tileY >= mapSizeY || tileZ < 0 || tileZ >= mapSizeZ)
	{
		return true;
	}
	// Inside something solid: the voxel at the point (counted within the tile) is filled in one
	// of the tile's four parts (floor, west wall, north wall, object).
	const TileView &tileView = _tileViews[(tileZ * mapSizeY + tileY) * mapSizeX + tileX];
	const int voxelX = std::clamp((int)x - tileX * 16, 0, 15), voxelY = std::clamp((int)y - tileY * 16, 0, 15),
		voxelZ = std::clamp((int)z - tileZ * 24, 0, 23);
	for (const VoxelModel *model : tileView.part)
	{
		if (model && model->color[voxelZ][voxelY][voxelX]) return false;
	}
	// A floor on the level right above is a roof: the point is indoors.
	if (tileZ + 1 < mapSizeZ && _tileViews[((tileZ + 1) * mapSizeY + tileY) * mapSizeX + tileX].part[O_FLOOR])
	{
		return false;
	}
	return true;
}

/**
 * Finds the colour of the terrain at a point, or right next to it, so the chunks thrown from
 * a hit or a destroyed tile part look like what was hit. The voxel at the point is looked at
 * first, then the cube of voxels one step around it.
 * @param pos The point, in voxels.
 * @return The palette colour of the first solid voxel found, or the smoke colour (a neutral
 * grey) if there is none.
 */
Uint8 FirstPersonView::debrisColorAt(const float pos[3]) const
{
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	// Search outwards: the voxel itself (reach 0), then the 3x3x3 voxels around it (reach 1).
	for (int reach = 0; reach <= 1; ++reach)
	{
		// dx, dy, dz: the offset from the point, in voxels.
		for (int dz = -reach; dz <= reach; ++dz)
		for (int dy = -reach; dy <= reach; ++dy)
		for (int dx = -reach; dx <= reach; ++dx)
		{
			// The voxel to look at and the tile it is in; skip it if it is off the map.
			const int voxelX = (int)pos[0] + dx, voxelY = (int)pos[1] + dy, voxelZ = (int)pos[2] + dz;
			const int tileX = voxelX / 16, tileY = voxelY / 16, tileZ = voxelZ / 24;
			if (voxelX < 0 || voxelY < 0 || voxelZ < 0 || tileX >= mapSizeX || tileY >= mapSizeY || tileZ >= mapSizeZ)
			{
				continue;
			}
			// The first of the tile's parts (floor, west wall, north wall, object) with a solid
			// voxel there gives the colour.
			const TileView &tileView = _tileViews[(tileZ * mapSizeY + tileY) * mapSizeX + tileX];
			for (int part = 0; part < 4; ++part)
			{
				if (tileView.part[part])
				{
					const Uint8 color = tileView.part[part]->color[voxelZ % 24][voxelY % 16][voxelX % 16];
					if (color) return color;
				}
			}
		}
	}
	return _colors.smoke;
}

/**
 * Spawns the particles for what happens in the battle this frame: the shot in flight (a muzzle
 * flash and smoke when it leaves, its trail as it flies, and where it lands sparks, debris in
 * the colour of what was hit and a mark; see followShots), the flash, embers and smoke of new
 * explosions, flames from burning tiles, drifting smoke from smoky ones, and flames licking
 * over burning units.
 * @param elapsedSeconds Seconds since the last frame (fire and smoke spawn at rates per second).
 */
void FirstPersonView::spawnBattleEffects(float elapsedSeconds)
{
	followShots();
	spawnExplosionEffects();
	spawnTileFireAndSmoke(elapsedSeconds);
	spawnBurningUnitFlames(elapsedSeconds);
}

/**
 * Reacts to the explosions that appeared on the map since the last frame (each one only
 * once). The camera shakes if one is close. A big one (a grenade, a rocket, a blown-up tile)
 * blackens the ground around it and throws out a flash, embers and smoke; a small one (a
 * bullet's hit) throws a few sparks.
 *
 * The particles are thrown with FirstPersonParticles::burst, whose arguments are, in order:
 * where, how many, kind, group, the colours and how many of them, the slowest and fastest
 * speed (voxels per second), the upward bias of the directions, gravity (voxels per second
 * squared, negative rises), drag (share of the speed lost per second), the shortest and
 * longest life (seconds), and the size (pixels at one tile's distance).
 */
void FirstPersonView::spawnExplosionEffects()
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// The explosions on the map this frame; next frame, these count as already seen.
	std::set<const Explosion*> currentExplosions;
	if (_map)
	{
		for (const Explosion *explosion : *_map->getExplosions())
		{
			currentExplosions.insert(explosion);
			if (_seenExplosions.count(explosion))
			{
				continue;
			}
			// A new explosion: where it is (in voxels), and how close to the eye. Closeness is 1
			// at the eye and falls to 0 at 260 voxels (about 16 tiles) for a big one, 90 voxels
			// for a small one; within that reach the camera shakes, harder the closer it is.
			const Position position = explosion->getPosition();
			const float centre[3] = { (float)position.x, (float)position.y, (float)position.z };
			const float dx = centre[0] - (float)_eye[0], dy = centre[1] - (float)_eye[1], dz = centre[2] - (float)_eye[2]; // from the eye
			const float closeness = 1.0f - std::sqrt(dx * dx + dy * dy + dz * dz) / (explosion->isBig() ? 260.0f : 90.0f);
			if (closeness > 0.0f)
			{
				addShake(closeness * (explosion->isBig() ? 3.0f : 1.0f));
			}
			if (explosion->isBig())
			{
				// The ground around it is blackened: a scorch of 14 voxels' radius on top of its
				// level's floor, if it went off near the floor (up to 60 scorches are kept).
				const float floorZ = std::floor(centre[2] / 24.0f) * 24.0f + GroundTop;
				if (centre[2] - floorZ < 18.0f && _scorches.size() < 60)
				{
					_scorches.push_back({ { centre[0], centre[1], floorZ }, 14.0f });
				}
				const Uint8 smokeColors[2] = { _colors.smoke, shadeColor(_colors.smoke, 3) };
				// The flash: a few big blobs in the two hottest fire colours, barely moving and
				// gone within a fifth of a second.
				_particles.burst(centre, 18, P::GLOW, P::EFFECT, _colors.fire, 2, 10, 40, 0, 0, 3, 0.08f, 0.2f, 6);
				// Embers in all four fire colours, flung out fast (mostly upwards), falling back
				// and glowing for a second or so.
				_particles.burst(centre, 60, P::GLOW, P::EFFECT, _colors.fire, 4, 40, 140, 0.5f, 150, 0.6f, 0.6f, 1.5f, 2);
				// Big puffs of smoke in two greys, rising slowly and hanging for a few seconds.
				_particles.burst(centre, 26, P::LIT, P::EFFECT, smokeColors, 2, 6, 24, 0.7f, -12, 0.8f, 2.0f, 3.5f, 5);
			}
			else
			{
				// A small explosion (a hit): a few sparks, falling and gone within half a second.
				const Uint8 sparkColors[2] = { _colors.spark, _colors.fire[1] };
				_particles.burst(centre, 8, P::GLOW, P::EFFECT, sparkColors, 2, 20, 60, 0.3f, 120, 1.0f, 0.15f, 0.4f, 2);
			}
		}
	}
	_seenExplosions.swap(currentExplosions);
}

/**
 * Burning tiles near the camera send up flames, and smoky ones drifting smoke (a burning tile
 * smokes too). Only tiles that have been discovered and are within 320 voxels (20 tiles) of
 * the eye, across the map, take part.
 * @param elapsedSeconds Seconds since the last frame: flames and smoke rise at rates per second.
 */
void FirstPersonView::spawnTileFireAndSmoke(float elapsedSeconds)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	const int tileCount = _save->getMapSizeXYZ();
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		Tile *tile = _save->getTile(tileIndex);
		if ((!tile->getFire() && !tile->getSmoke()) || !_tileViews[tileIndex].discovered)
		{
			continue;
		}
		// The middle of the tile, in voxels, a little above its floor (raised by the terrain
		// level); skip tiles too far from the eye.
		const Position tilePosition = tile->getPosition();
		const float centreX = tilePosition.x * 16 + 8.0f, centreY = tilePosition.y * 16 + 8.0f;
		const float groundZ = tilePosition.z * 24 - tile->getTerrainLevel() + 2.0f;
		if (std::abs(centreX - _eye[0]) > 320 || std::abs(centreY - _eye[1]) > 320)
		{
			continue;
		}
		// Fire: about ten flames a second, rising fast from somewhere on the tile and glowing
		// for a second or so in the fire colours.
		if (tile->getFire() && _particles.random() < 10.0f * elapsedSeconds)
		{
			P::Particle flame;
			flame.pos[0] = centreX + _particles.random(-6, 6);
			flame.pos[1] = centreY + _particles.random(-6, 6);
			flame.pos[2] = groundZ + _particles.random(0, 10);
			flame.vel[0] = _particles.random(-4, 4); flame.vel[1] = _particles.random(-4, 4); flame.vel[2] = _particles.random(14, 34);
			flame.age = 0; flame.life = _particles.random(0.7f, 1.6f); flame.gravity = -10; flame.drag = 0.8f;
			flame.color = _colors.fire[(int)_particles.random(0, 4)]; flame.size = 2; flame.kind = P::GLOW; flame.group = P::EFFECT;
			_particles.add(flame);
		}
		// Smoke: three puffs a second off a burning tile; off a smoky one 1.5 a second, more the
		// thicker its smoke. The puffs rise slowly and drift east (+x, the way the rain slants).
		if (tile->getSmoke() && _particles.random() < (tile->getFire() ? 3.0f : 1.5f + tile->getSmoke() * 0.2f) * elapsedSeconds)
		{
			P::Particle puff;
			puff.pos[0] = centreX + _particles.random(-7, 7);
			puff.pos[1] = centreY + _particles.random(-7, 7);
			puff.pos[2] = groundZ + _particles.random(6, 18);
			puff.vel[0] = _particles.random(-3, 3) + 2; puff.vel[1] = _particles.random(-3, 3); puff.vel[2] = _particles.random(3, 9);
			puff.age = 0; puff.life = _particles.random(2.0f, 3.5f); puff.gravity = -2; puff.drag = 0.4f;
			puff.color = shadeColor(_colors.smoke, (int)_particles.random(0, 4)); puff.size = 5; puff.kind = P::LIT; puff.group = P::EFFECT;
			_particles.add(puff);
		}
	}
}

/**
 * Burning units in sight (all of them in debug mode), but not our own soldier nor anyone
 * down: flames lick up all over them, more on a big unit, and smoke rises off the top. Units
 * more than 320 voxels (20 tiles) from the eye, across the map, are left out.
 * @param elapsedSeconds Seconds since the last frame: flames and smoke rise at rates per second.
 */
void FirstPersonView::spawnBurningUnitFlames(float elapsedSeconds)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	const Uint32 now = SDL_GetTicks();
	for (const BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getFire() <= 0 || unit->isOut() || unit == _cameraUnit || (!unit->getVisible() && !_save->getDebugMode()))
		{
			continue;
		}
		// Where the unit's feet are, in voxels: where it is drawn if it was drawn in the last
		// quarter second (it glides between the game's steps), else the middle of its tiles
		// on top of the floor. A 2x2 unit is two tiles across.
		const int unitSize = unit->getArmor()->getSize();
		float feet[3];
		auto glide = _unitGlides.find(unit);
		if (glide != _unitGlides.end() && now - glide->second.seen < 250)
		{
			for (int axis = 0; axis < 3; ++axis) feet[axis] = (float)glide->second.pos[axis];
		}
		else
		{
			const Position tilePosition = unit->getPosition();
			const Tile *tile = _save->getTile(tilePosition);
			feet[0] = tilePosition.x * 16 + 8.0f * unitSize;
			feet[1] = tilePosition.y * 16 + 8.0f * unitSize;
			feet[2] = tilePosition.z * 24 - (tile ? tile->getTerrainLevel() : 0);
		}
		if (std::abs(feet[0] - _eye[0]) > 320 || std::abs(feet[1] - _eye[1]) > 320)
		{
			continue;
		}
		// Flames: 45 a second per tile of the unit's size (the fraction carried by chance), from
		// anywhere on its body up to nine tenths of its height, rising fast and short-lived.
		const float height = (float)(unit->getHeight() + unit->getFloatHeight()); // in voxels, off the ground
		const int flameCount = (int)(elapsedSeconds * 45.0f * unitSize + _particles.random());
		for (int k = 0; k < flameCount; ++k)
		{
			P::Particle flame;
			flame.pos[0] = feet[0] + _particles.random(-4, 4) * unitSize; flame.pos[1] = feet[1] + _particles.random(-4, 4) * unitSize;
			flame.pos[2] = feet[2] + height * _particles.random(0.05f, 0.9f);
			flame.vel[0] = _particles.random(-4, 4); flame.vel[1] = _particles.random(-4, 4); flame.vel[2] = _particles.random(16, 36);
			flame.age = 0; flame.life = _particles.random(0.35f, 0.8f); flame.gravity = -12; flame.drag = 0.8f;
			flame.color = _colors.fire[(int)_particles.random(0, 3.99f)]; flame.size = 3; flame.kind = P::GLOW; flame.group = P::EFFECT;
			_particles.add(flame);
		}
		// Smoke: about five puffs a second off the top of the unit, rising and drifting east (+x,
		// the way the rain slants).
		if (_particles.random() < elapsedSeconds * 5.0f)
		{
			P::Particle puff;
			puff.pos[0] = feet[0] + _particles.random(-3, 3); puff.pos[1] = feet[1] + _particles.random(-3, 3);
			puff.pos[2] = feet[2] + height + 2.0f;
			puff.vel[0] = _particles.random(-3, 3) + 2; puff.vel[1] = _particles.random(-3, 3); puff.vel[2] = _particles.random(6, 12);
			puff.age = 0; puff.life = _particles.random(1.5f, 2.5f); puff.gravity = -3; puff.drag = 0.4f;
			puff.color = shadeColor(_colors.smoke, (int)_particles.random(1, 4)); puff.size = 5; puff.kind = P::LIT; puff.group = P::EFFECT;
			_particles.add(puff);
		}
	}
}

/**
 * Keeps the air alive around the camera: dust motes by day, fireflies outside at night,
 * bubbles under the sea, and the terrain's weather (rain, snow, blowing sand) where the sky
 * is open above. Under the sea there are also columns of bubbles rising from a few spots on
 * the sea floor, and marine snow (specks hanging in the water) instead of weather.
 * Particles the camera has left behind are dropped and new ones fill in around it, so the
 * camera seems to move through endless air (or water).
 * @param elapsedSeconds Seconds since the last frame: the spawn rates are per second.
 */
void FirstPersonView::spawnAmbient(float elapsedSeconds)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	// The weather is chosen once per battle: the first of the battle's terrain data sets that
	// the mod gives a weather (rain, snow, or dust for blowing sand), else none.
	if (!_weatherChosen)
	{
		_weatherChosen = true;
		_weather.clear();
		for (const MapDataSet *mapDataSet : *_save->getMapDataSets())
		{
			const std::string weather = _game->getMod()->getFirstPerson().getWeather(mapDataSet->getName());
			if (!weather.empty())
			{
				_weather = weather;
				break;
			}
		}
	}
	const float eye[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
	// What the camera left behind (more than 130 or 150 voxels away across the map) is gone,
	// and comes back around it.
	_particles.cull(P::AMBIENT, eye[0], eye[1], 130);
	_particles.cull(P::WEATHER, eye[0], eye[1], 150);

	// Top up the ambient particles: 600 bubbles all through the water under the sea, else 90
	// dust motes by day, or 30 fireflies at night. They appear anywhere within 90 voxels of
	// the eye east-west and north-south.
	const int ambientTarget = _colors.underwater ? 600 : (_colors.night > 0.6f ? 30 : 90);
	for (int ambientCount = _particles.count(P::AMBIENT); ambientCount < ambientTarget; ++ambientCount)
	{
		P::Particle particle;
		const float spread = 90.0f;
		particle.pos[0] = eye[0] + _particles.random(-spread, spread);
		particle.pos[1] = eye[1] + _particles.random(-spread, spread);
		if (_colors.underwater && _particles.random() < 0.6f)
		{
			// Most of the bubbles go where the camera looks (within about 70 degrees to
			// either side), at least 8 voxels ahead.
			const float yaw = (float)_yaw + _particles.random(-1.2f, 1.2f), distance = _particles.random(8.0f, spread);
			particle.pos[0] = eye[0] + std::cos(yaw) * distance;
			particle.pos[1] = eye[1] + std::sin(yaw) * distance;
		}
		// The height: under the sea anywhere from the sea floor to just above the eye; otherwise
		// from a little below the eye to a little above it.
		particle.pos[2] = _colors.underwater ? _particles.random(GroundTop, eye[2] + 12.0f) : eye[2] + _particles.random(-18, 30);
		particle.age = 0;
		particle.group = P::AMBIENT;
		particle.size = 1;
		if (_colors.underwater)
		{
			// A bubble, rising and wobbling for a few seconds.
			if (!inWater(particle.pos[0], particle.pos[1], particle.pos[2])) continue; // not inside the sub, a building or a rock
			particle.vel[0] = _particles.random(-2, 2); particle.vel[1] = _particles.random(-2, 2);
			particle.vel[2] = _particles.random(6, 16);
			particle.life = _particles.random(2.5f, 5.0f); particle.gravity = 0; particle.drag = 0;
			particle.color = _colors.bubble; particle.kind = P::BUBBLE;
			particle.size = (Uint8)(2 + (int)(_particles.random() * 3.0f)); // 2..4
		}
		else if (_colors.night > 0.6f)
		{
			// A firefly, only under the open sky, wandering and blinking for a few seconds.
			if (!openSky(particle.pos[0], particle.pos[1], particle.pos[2])) continue;
			particle.vel[0] = _particles.random(-4, 4); particle.vel[1] = _particles.random(-4, 4);
			particle.vel[2] = _particles.random(-2, 2);
			particle.life = _particles.random(3, 8); particle.gravity = 0; particle.drag = 0;
			particle.color = _colors.firefly; particle.kind = P::BLINK;
		}
		else
		{
			// A dust mote, lit by the light where it is, barely drifting.
			particle.vel[0] = _particles.random(-1.5f, 1.5f); particle.vel[1] = _particles.random(-1.5f, 1.5f);
			particle.vel[2] = _particles.random(-0.8f, 0.8f);
			particle.life = _particles.random(4, 9); particle.gravity = 0; particle.drag = 0;
			particle.color = _colors.dust; particle.kind = P::LIT;
		}
		_particles.add(particle);
	}

	if (_colors.underwater)
	{
		// Columns of bubbles from a few spots (vents) on the sea floor around the camera. The
		// vents are chosen again when the camera has moved more than 90 voxels from their
		// middle: up to 7 spots in open water within 110 voxels, out of 24 tries.
		float ventsCentreX = 0, ventsCentreY = 0;
		for (const auto &vent : _bubbleVents) { ventsCentreX += vent.first; ventsCentreY += vent.second; }
		if (!_bubbleVents.empty()) { ventsCentreX /= _bubbleVents.size(); ventsCentreY /= _bubbleVents.size(); }
		if (_bubbleVents.empty()
			|| (ventsCentreX - eye[0]) * (ventsCentreX - eye[0]) + (ventsCentreY - eye[1]) * (ventsCentreY - eye[1]) > 90.0f * 90.0f)
		{
			_bubbleVents.clear();
			for (int attempt = 0; attempt < 24 && _bubbleVents.size() < 7; ++attempt)
			{
				const float x = eye[0] + _particles.random(-110, 110), y = eye[1] + _particles.random(-110, 110);
				if (inWater(x, y, GroundTop + 1.0f)) _bubbleVents.push_back({ x, y });
			}
		}
		// Each vent lets out about nine bubbles a second, rising straight up for a few seconds.
		for (const auto &vent : _bubbleVents)
		{
			if (_particles.random() > elapsedSeconds * 9.0f) continue;
			P::Particle bubble;
			bubble.pos[0] = vent.first + _particles.random(-1.5f, 1.5f);
			bubble.pos[1] = vent.second + _particles.random(-1.5f, 1.5f);
			bubble.pos[2] = GroundTop + 1.0f;
			bubble.vel[0] = _particles.random(-1.5f, 1.5f); bubble.vel[1] = _particles.random(-1.5f, 1.5f);
			bubble.vel[2] = _particles.random(14, 22);
			bubble.age = 0; bubble.life = _particles.random(4, 6); bubble.gravity = 0; bubble.drag = 0;
			bubble.color = _colors.bubble; bubble.size = (Uint8)(2 + (int)(_particles.random() * 2.0f)); // 2..3
			bubble.kind = P::BUBBLE; bubble.group = P::EFFECT;
			_particles.add(bubble);
		}
		// Marine snow: 1200 specks hanging in the water, drifting with the current, within 100
		// voxels of the eye. Below half of that, they all come back at once (after a jump, or
		// at the start); otherwise they are topped up a little each frame.
		const int target = 1200;
		const int have = _particles.count(P::WEATHER);
		const int toSpawn = have < target / 2 ? target - have : std::min(target - have, (int)(target * elapsedSeconds * 0.3f) + 1);
		for (int n = 0; n < toSpawn; ++n)
		{
			P::Particle speck;
			// Most of them where the camera looks, and about eye high (the square root spreads
			// them evenly over the disc).
			const float angle = _particles.random() < 0.6f ? (float)_yaw + _particles.random(-1.2f, 1.2f)
				: _particles.random(0, 2 * (float)Pi);
			const float radius = 100 * std::sqrt(_particles.random());
			speck.pos[0] = eye[0] + std::cos(angle) * radius;
			speck.pos[1] = eye[1] + std::sin(angle) * radius;
			speck.pos[2] = _particles.random(GroundTop, eye[2] + 25.0f);
			if (!inWater(speck.pos[0], speck.pos[1], speck.pos[2])) continue;
			speck.vel[0] = _particles.random(0.5f, 3.0f); speck.vel[1] = _particles.random(-1.5f, 1.5f);
			speck.vel[2] = _particles.random(-1.5f, -0.3f);
			speck.age = 0; speck.life = _particles.random(6, 11); speck.gravity = 0; speck.drag = 0;
			speck.color = _colors.marine; speck.size = 1; speck.kind = P::LIT; speck.group = P::WEATHER;
			_particles.add(speck);
		}
		return;
	}

	// The weather, if the terrain has any: 1500 raindrops, 1400 snowflakes or 300 grains of
	// blowing sand around the camera.
	if (_weather.empty())
	{
		return;
	}
	const bool rain = _weather == "rain", snow = _weather == "snow";
	const int target = rain ? 1500 : snow ? 1400 : 300;
	const int have = _particles.count(P::WEATHER);
	// Spread over time so they don't land together, but fill up at once after a jump.
	const int toSpawn = have < target / 2 ? target - have : std::min(target - have, (int)(target * elapsedSeconds * 1.5f) + 1);
	for (int n = 0; n < toSpawn; ++n)
	{
		P::Particle particle;
		// Evenly over a disc of 130 voxels around the camera (the square root spreads them
		// evenly): most of what's seen falls close by.
		const float angle = _particles.random(0, 2 * (float)Pi), radius = 130 * std::sqrt(_particles.random());
		particle.pos[0] = eye[0] + std::cos(angle) * radius;
		particle.pos[1] = eye[1] + std::sin(angle) * radius;
		particle.age = 0;
		particle.group = P::WEATHER;
		particle.drag = 0;
		particle.gravity = 0;
		if (rain || snow)
		{
			// Rain or snow: from somewhere between 25 voxels below the eye (not below the
			// ground) and 45 above it, falling to the ground (rain fast and slanting, snow slowly
			// and wandering), and gone when it gets there.
			particle.pos[2] = _particles.random(std::max((float)GroundTop, eye[2] - 25), eye[2] + 45);
			const float fallSpeed = rain ? _particles.random(260, 320) : _particles.random(18, 30); // voxels per second
			particle.vel[0] = rain ? 30 : _particles.random(-6, 6); particle.vel[1] = rain ? 10 : _particles.random(-6, 6);
			particle.vel[2] = -fallSpeed;
			particle.life = (particle.pos[2] - GroundTop) / fallSpeed;
			// Flakes in two tones, so they read against white clouds and snow as well as blue sky.
			particle.color = rain ? _colors.rain : (_particles.random() < 0.5f ? _colors.snow : shadeColor(_colors.snow, 3));
			particle.size = rain ? 1 : 5;
			particle.kind = rain ? P::STREAK : P::LIT;
		}
		else
		{
			// Blowing sand: low over the ground, blown fast along the wind (east, a little south).
			particle.pos[2] = GroundTop + _particles.random(0, 40);
			particle.vel[0] = _particles.random(40, 70); particle.vel[1] = _particles.random(5, 20);
			particle.vel[2] = _particles.random(-3, 3);
			particle.life = _particles.random(2, 4);
			particle.color = _colors.sand;
			particle.size = 1;
			particle.kind = P::LIT;
		}
		// Nothing falls under a roof: the sky must be open above the point. Particles don't
		// stop at a roof, so the check starts 100 voxels lower down: a drop spawned above a
		// roof would fall through it, and is left out too.
		if (!openSky(particle.pos[0], particle.pos[1], std::max(GroundTop, particle.pos[2] - 100)))
		{
			continue; // under a roof
		}
		_particles.add(particle);
	}
}

/**
 * Brings the machinery and lights to life: energy spiralling up off UFO power sources with
 * the odd crackle of sparks, status lights blinking on navigation consoles, and (at night or
 * under the sea) motes drifting in the glow of lamps. The devices and lamps are the ones near
 * the camera, found while taking the tile snapshot (see noteLightsAndDevices).
 * @param elapsedSeconds Seconds since the last frame: the spawn rates are per second.
 */
void FirstPersonView::spawnDevices(float elapsedSeconds)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	for (const auto &device : _nearbyDevices)
	{
		if (device.power)
		{
			// A power source: about 50 wisps of energy a second spiralling up around the core,
			// just off its surface (the fraction carried by chance), in the two hottest fire
			// colours.
			const int wispCount = (int)(elapsedSeconds * 50.0f + _particles.random());
			const float radius = std::max(device.half[0], device.half[1]) * 0.9f + 1.5f;
			for (int i = 0; i < wispCount; ++i)
			{
				P::Particle wisp;
				// A random point on a ring around the core, at any height along it.
				const float angle = _particles.random(0, 2 * (float)Pi);
				const float cosAngle = std::cos(angle), sinAngle = std::sin(angle);
				wisp.pos[0] = device.pos[0] + cosAngle * radius;
				wisp.pos[1] = device.pos[1] + sinAngle * radius;
				wisp.pos[2] = device.pos[2] + _particles.random(-device.half[2], device.half[2]);
				// It swirls around the core (along the ring), drifts a little outwards and rises.
				const float swirl = _particles.random(5, 9); // voxels per second
				wisp.vel[0] = -sinAngle * swirl + cosAngle * 1.5f; wisp.vel[1] = cosAngle * swirl + sinAngle * 1.5f;
				wisp.vel[2] = _particles.random(9, 18);
				wisp.age = 0; wisp.life = _particles.random(1.0f, 2.2f); wisp.gravity = 0; wisp.drag = 0.3f;
				wisp.color = _particles.random() < 0.5f ? _colors.fire[0] : _colors.fire[1];
				wisp.size = _particles.random() < 0.4f ? 3 : 2;
				wisp.kind = P::GLOW; wisp.group = P::EFFECT;
				_particles.add(wisp);
			}
			// Now and then (0.7 times a second) a crackle: ten small sparks out of its upper
			// half, flying out at 20..50 voxels per second, falling, gone within 0.6 seconds.
			if (_particles.random() < elapsedSeconds * 0.7f)
			{
				const float crackleAt[3] = { device.pos[0], device.pos[1], device.pos[2] + device.half[2] * 0.5f };
				const Uint8 sparkColors[2] = { _colors.spark, _colors.fire[0] };
				_particles.burst(crackleAt, 10, P::GLOW, P::EFFECT, sparkColors, 2, 20, 50, 0.5f, 60, 0.5f, 0.2f, 0.6f, 1);
			}
		}
		else if (_particles.random() < elapsedSeconds * 4.0f)
		{
			// A navigation console: about four times a second, a status light blinks on
			// somewhere on top of it, red, green or blue: the palette colour nearest to one of
			// these (red, green, blue values).
			static const Uint8 statusColors[3][3] = { { 240, 60, 40 }, { 80, 230, 90 }, { 90, 160, 255 } };
			const Uint8 *tint = statusColors[(int)(_particles.random() * 2.999f)];
			const SDL_Color *palette = getPalette();
			int best = 1, bestDistance = INT_MAX;
			for (int index = 1; index < 256; ++index)
			{
				// How far the palette colour is from the wanted one, in red, green and blue.
				const int dr = palette[index].r - tint[0], dg = palette[index].g - tint[1], db = palette[index].b - tint[2];
				if (dr * dr + dg * dg + db * db < bestDistance) { bestDistance = dr * dr + dg * dg + db * db; best = index; }
			}
			// The light sits still on the console's top for a couple of seconds, blinking.
			P::Particle statusLight;
			statusLight.pos[0] = device.pos[0] + _particles.random(-device.half[0], device.half[0]) * 0.8f;
			statusLight.pos[1] = device.pos[1] + _particles.random(-device.half[1], device.half[1]) * 0.8f;
			statusLight.pos[2] = device.pos[2] + device.half[2] + 0.5f;
			statusLight.vel[0] = statusLight.vel[1] = statusLight.vel[2] = 0;
			statusLight.age = 0; statusLight.life = _particles.random(1.5f, 3.5f); statusLight.gravity = 0; statusLight.drag = 0;
			statusLight.color = (Uint8)best; statusLight.size = 2; statusLight.kind = P::BLINK; statusLight.group = P::EFFECT;
			_particles.add(statusLight);
		}
	}
	// Motes in the lamplight (at night or under the sea): around each lamp within 200 voxels
	// across the map, more around a brighter one (up to 2.5 a second), self-lit in the lamp's
	// colour, wandering for a couple of seconds.
	if (_colors.night > 0.4f || _colors.underwater)
	{
		for (const auto &light : _nearbyLights)
		{
			const float dx = light.pos[0] - (float)_eye[0], dy = light.pos[1] - (float)_eye[1]; // from the eye to the lamp
			if (dx * dx + dy * dy > 200.0f * 200.0f || _particles.random() > elapsedSeconds * 2.5f * light.strength) continue;
			P::Particle mote;
			mote.pos[0] = light.pos[0] + _particles.random(-6, 6);
			mote.pos[1] = light.pos[1] + _particles.random(-6, 6);
			mote.pos[2] = light.pos[2] + _particles.random(-6, 4);
			mote.vel[0] = _particles.random(-4, 4); mote.vel[1] = _particles.random(-4, 4); mote.vel[2] = _particles.random(-2, 3);
			mote.age = 0; mote.life = _particles.random(1.5f, 3.5f); mote.gravity = 0; mote.drag = 0;
			mote.color = light.color; mote.size = 1; mote.kind = P::GLOW; mote.group = P::EFFECT;
			_particles.add(mote);
		}
	}
}

/**
 * Bloom: whatever shines (the lit parts of lamps and screens, sparks, embers, flashes, beams)
 * spills a soft glow over its surroundings. The shining pixels (marked in _glowMask by the
 * ray caster and the effects drawn after it) are gathered into a grid at quarter resolution,
 * blurred, and the image is brightened where that glow lands: each pixel by up to three
 * shades within its own 16-colour group, dithered, so the palette still works. It shows most
 * at night and under the sea.
 */
void FirstPersonView::drawLightHalos()
{
	// The glow grid: one cell for every 4x4 pixels of the view.
	const int width = getWidth(), height = getHeight();
	const int cellSize = 4, gridWidth = (width + cellSize - 1) / cellSize, gridHeight = (height + cellSize - 1) / cellSize;
	_glowBuffer.assign(gridWidth * gridHeight, 0.0f);
	_glowScratch.assign(gridWidth * gridHeight, 0.0f);
	// Gather: each cell holds the share of its pixels that shine (0..1). Nothing shining,
	// nothing to do.
	bool anyGlow = false;
	for (int y = 0; y < height; ++y)
	{
		const Uint8 *maskRow = &_glowMask[y * width];
		float *glowRow = &_glowBuffer[(y / cellSize) * gridWidth];
		for (int x = 0; x < width; ++x)
		{
			if (maskRow[x])
			{
				glowRow[x / cellSize] += 1.0f / (cellSize * cellSize);
				anyGlow = true;
			}
		}
	}
	if (!anyGlow)
	{
		return;
	}
	// Blur: two passes of a box blur 7 cells wide, each across then down (through the scratch
	// grid), which comes close to a gaussian.
	const int radius = 3;
	for (int pass = 0; pass < 2; ++pass)
	{
		for (int y = 0; y < gridHeight; ++y)
		{
			for (int x = 0; x < gridWidth; ++x)
			{
				float sum = 0.0f;
				for (int offset = -radius; offset <= radius; ++offset)
				{
					sum += _glowBuffer[y * gridWidth + std::clamp(x + offset, 0, gridWidth - 1)];
				}
				_glowScratch[y * gridWidth + x] = sum / (2 * radius + 1);
			}
		}
		for (int y = 0; y < gridHeight; ++y)
		{
			for (int x = 0; x < gridWidth; ++x)
			{
				float sum = 0.0f;
				for (int offset = -radius; offset <= radius; ++offset)
				{
					sum += _glowScratch[std::clamp(y + offset, 0, gridHeight - 1) * gridWidth + x];
				}
				_glowBuffer[y * gridWidth + x] = sum / (2 * radius + 1);
			}
		}
	}
	// How strongly the glow brightens: more at night, and most under the sea. It is steady
	// (a flickering bloom makes the dithered glow shimmer).
	const float gain = _colors.underwater ? 45.0f : 18.0f + 42.0f * _colors.night;
	// Brighten every pixel the glow reaches, except the shining pixels themselves and empty
	// (black, index 0) ones. The glow at a pixel is blended between the four nearest cells.
	for (int y = 0; y < height; ++y)
	{
		Uint8 *pixels = getRaw(0, y);
		// The pixel row's place in the grid: the cell row above it and how far towards the next.
		const float gridY = std::clamp((y + 0.5f) / cellSize - 0.5f, 0.0f, gridHeight - 1.001f);
		const int gridRow = (int)gridY;
		const float blendY = gridY - gridRow;
		const float *glowAbove = &_glowBuffer[gridRow * gridWidth],
			*glowBelow = &_glowBuffer[std::min(gridHeight - 1, gridRow + 1) * gridWidth];
		for (int x = 0; x < width; ++x)
		{
			if (_glowMask[y * width + x] || !pixels[x]) continue;
			// The same across: the cell column to the left of the pixel and how far towards the next.
			const float gridX = std::clamp((x + 0.5f) / cellSize - 0.5f, 0.0f, gridWidth - 1.001f);
			const int gridColumn = (int)gridX;
			const float blendX = gridX - gridColumn;
			const int nextColumn = std::min(gridWidth - 1, gridColumn + 1);
			const float glow = (glowAbove[gridColumn] * (1 - blendX) + glowAbove[nextColumn] * blendX) * (1 - blendY)
				+ (glowBelow[gridColumn] * (1 - blendX) + glowBelow[nextColumn] * blendX) * blendY;
			// The glow in shades, dithered: one shade brighter from a dithered threshold up, two
			// past 1, three past 2. A lower index within a colour group is a brighter shade.
			const float strength = glow * gain;
			if (strength <= dither(x, y)) continue;
			const int steps = strength > 2.0f + dither(x + 1, y + 2) ? 3 : strength > 1.0f + dither(x + 1, y + 2) ? 2 : 1;
			pixels[x] = (Uint8)((pixels[x] & 0xF0) | std::max(0, (pixels[x] & 15) - steps));
		}
	}
}

/**
 * Advances the particles by the time since the last frame and spawns the new ones, once per
 * frame before drawing. With particles off for this view everything is cleared; with the
 * particles option at 0 only the tactical flares still fly (and shine, without their
 * particles); the ambient ones (motes, fireflies, weather, bubbles) need the option above 1.
 */
void FirstPersonView::updateParticles()
{
	// Seconds since the last frame, at most a tenth of a second (after a pause the particles
	// don't jump all at once), and none on the very first frame.
	const Uint32 now = SDL_GetTicks();
	const float elapsedSeconds = _particleLastTick ? std::min<Uint32>(100, now - _particleLastTick) / 1000.0f : 0.0f;
	_particleLastTick = now;
	if (!_particlesOn)
	{
		_particles.clear();
		return;
	}
	if (Options::fpsParticles == 0)
	{
		_particles.clear();
		updateFlares(elapsedSeconds); // flares still fly and shine without their particles
		return;
	}
	// Move what's there, then add what this frame brings: the battle's effects (shots,
	// explosions, fire), dual wield's second round, the UFO devices and lamps, and the air.
	_particles.update(elapsedSeconds);
	updateFlares(elapsedSeconds);
	spawnBattleEffects(elapsedSeconds);
	spawnPartnerEffects();
	spawnDevices(elapsedSeconds);
	if (Options::fpsParticles > 1)
	{
		spawnAmbient(elapsedSeconds);
	}
}

/**
 * Draws the particles: square dots (bigger up close, with the corners rounded off), rings for
 * bubbles up close, or streaks for rain, all depth-tested against the scene. Self-lit ones
 * (GLOW, BLINK) darken as they age and feed the bloom; the rest (LIT, BUBBLE) are lit by the
 * battlescape light where they are.
 */
void FirstPersonView::drawParticles()
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds (P::GLOW...) and groups (P::EFFECT...)
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // in pixels
	for (const auto &particle : _particles.all())
	{
		// Where it is on the screen (skip it if it's behind the camera or off the screen).
		double world[3] = { particle.pos[0], particle.pos[1], particle.pos[2] };
		if (particle.kind == P::BUBBLE)
		{
			// A bubble wobbles from side to side as it rises.
			const float phase = particle.age * 5.0f + particle.pos[0] * 0.7f + particle.pos[1] * 0.3f;
			world[0] += std::sin(phase) * 0.9;
			world[1] += std::cos(phase * 0.8f) * 0.9;
		}
		double screenX, screenY, depth;
		if (!projectToScreen(world, screenX, screenY, depth)
			|| screenX < -4 || screenY < -4 || screenX >= width + 4 || screenY >= height + 4)
		{
			continue;
		}
		// Its shade (0 brightest .. 15 darkest within its colour group).
		const float fade = particle.age / particle.life; // how far through its life: 0 new .. 1 gone
		int shade;
		if (particle.kind == P::LIT || particle.kind == P::BUBBLE)
		{
			// Lit by the light where it is; an effect's particle (smoke, debris) also darkens
			// as it ages. Bubbles and specks in the water catch and scatter what light there
			// is: shade them less.
			const float scatter = particle.kind == P::BUBBLE ? 0.3f : (_colors.underwater && particle.group == P::WEATHER ? 0.45f : 1.0f);
			shade = (int)(std::clamp(lightAt(particle.pos[0], particle.pos[1], particle.pos[2]), 0.0f, 15.0f) * scatter
				+ (particle.group == P::EFFECT ? fade * 4.0f : 0.0f));
		}
		else
		{
			// Self-lit: it darkens as it ages. A blinking one is off for part of each blink
			// (its own rhythm, from where it is).
			if (particle.kind == P::BLINK && std::sin(particle.age * 5.0f + particle.pos[0]) < 0.3f) continue;
			shade = (int)(fade * 6.0f);
		}
		const Uint8 color = shadeColor(particle.color, shade);
		if (particle.kind == P::STREAK)
		{
			// A streak (rain): a line from the particle back to where it was 0.03 seconds ago.
			const double tail[3] = { particle.pos[0] - particle.vel[0] * 0.03, particle.pos[1] - particle.vel[1] * 0.03,
				particle.pos[2] - particle.vel[2] * 0.03 };
			double tailX, tailY, tailDepth;
			if (!projectToScreen(tail, tailX, tailY, tailDepth)) continue;
			const int steps = std::max(1, (int)std::max(std::abs(tailX - screenX), std::abs(tailY - screenY)));
			for (int step = 0; step <= steps; ++step)
			{
				const int x = (int)(screenX + (tailX - screenX) * step / steps), y = (int)(screenY + (tailY - screenY) * step / steps);
				if (x < 0 || y < 0 || x >= width || y >= height || depth >= _depthBuffer[y * width + x]) continue;
				*getRaw(x, y) = color;
			}
			continue;
		}
		// Its size on the screen in pixels: its size at one tile's distance, shrinking with
		// distance (bubbles are drawn a bit bigger, up to 9 pixels; the rest up to 6).
		const int dotSize = std::clamp((int)std::lround(particle.size * focalLength / depth * (particle.kind == P::BUBBLE ? 0.18 : 0.12)),
			1, particle.kind == P::BUBBLE ? 9 : 6);
		if (particle.kind == P::BUBBLE && dotSize >= 3)
		{
			// A bubble up close: a ring with a glint on its upper left.
			const double ringRadius = dotSize * 0.5;
			for (int y = (int)(screenY - ringRadius) - 1; y <= (int)(screenY + ringRadius) + 1; ++y)
			{
				for (int x = (int)(screenX - ringRadius) - 1; x <= (int)(screenX + ringRadius) + 1; ++x)
				{
					if (x < 0 || y < 0 || x >= width || y >= height || depth >= _depthBuffer[y * width + x]) continue;
					// Only the pixels whose centres lie within a pixel of the ring's edge.
					const double distance = std::sqrt((x + 0.5 - screenX) * (x + 0.5 - screenX)
						+ (y + 0.5 - screenY) * (y + 0.5 - screenY));
					if (distance <= ringRadius && distance > ringRadius - 1.0)
					{
						*getRaw(x, y) = color;
					}
				}
			}
			// The glint: one pixel three shades brighter, up and left of the centre.
			const int glintX = (int)(screenX - ringRadius * 0.35), glintY = (int)(screenY - ringRadius * 0.35);
			if (glintX >= 0 && glintY >= 0 && glintX < width && glintY < height && depth < _depthBuffer[glintY * width + glintX])
			{
				*getRaw(glintX, glintY) = shadeColor(particle.color, std::max(0, shade - 3));
			}
			continue;
		}
		// Anything else: a square dot centred on the particle.
		const int left = (int)screenX - dotSize / 2, top = (int)screenY - dotSize / 2;
		for (int y = top; y < top + dotSize; ++y)
		{
			for (int x = left; x < left + dotSize; ++x)
			{
				// Round off the corners (of a dot 3 pixels or more across).
				if (dotSize >= 3 && (x == left || x == left + dotSize - 1) && (y == top || y == top + dotSize - 1)) continue;
				if (x < 0 || y < 0 || x >= width || y >= height || depth >= _depthBuffer[y * width + x]) continue;
				*getRaw(x, y) = color;
				if (particle.kind == P::GLOW || particle.kind == P::BLINK) _glowMask[y * width + x] = 1; // sparks and embers bloom too
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
