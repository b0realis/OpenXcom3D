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
 * FirstPersonViewFlares.cpp: tactical flares.
 *
 * Thrown from the shoulder, bouncing off walls, burning red for a minute and lighting the 3D
 * view around them, with shadows.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <SDL.h>
#include "../Engine/Options.h"
#include "../Savegame/BattleUnit.h" // not used here, but its PARTS_STRING constants are part of this file's object code
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonParticles.h"
#include "FirstPersonViewShared.h"
#include "FirstPersonViewRayWalk.h"

namespace OpenXcom
{

/**
 * Throws a tactical flare from the soldier's right shoulder, on an arc that comes down on the
 * target: up to ten tiles away, a farther target gets a flare that falls short. It may hit a
 * wall first, bounce and drop. Under the sea it's only a short, slow toss straight ahead.
 * Only so many flares burn at once (FlareMax): a new one puts out the oldest.
 * @param target Where to throw it, in voxels.
 */
void FirstPersonView::throwFlare(const float target[3])
{
	// It leaves the hand at the right shoulder: 3 voxels right, 2 down and 3 ahead of the eye.
	Flare flare;
	for (int axis = 0; axis < 3; ++axis)
	{
		flare.pos[axis] = (float)(_eye[axis] + _right[axis] * 3.0 - _up[axis] * 2.0 + _forward[axis] * 3.0);
	}
	if (_colors.underwater)
	{
		// Under the sea: a gentle toss along the view's direction on the level (28 voxels per
		// second forward, 6 up), whatever the target.
		const double forwardX = _forward[0], forwardY = _forward[1],
			flatLength = std::max(1e-6, std::sqrt(forwardX * forwardX + forwardY * forwardY));
		flare.vel[0] = (float)(forwardX / flatLength * 28.0);
		flare.vel[1] = (float)(forwardY / flatLength * 28.0);
		flare.vel[2] = 6.0f;
	}
	else
	{
		// On land: aim at the target, or at the point ten tiles towards it when it's farther.
		float toTarget[3] = { target[0] - flare.pos[0], target[1] - flare.pos[1], target[2] - flare.pos[2] };
		float distance = std::sqrt(toTarget[0] * toTarget[0] + toTarget[1] * toTarget[1] + toTarget[2] * toTarget[2]);
		const float reach = 160.0f; // ten tiles: a strong throw
		if (distance > reach)
		{
			for (float &component : toTarget) component *= reach / distance;
			distance = reach;
		}
		// The flight takes a second per ten tiles (a quarter second at least): the speed covers
		// the distance in that time, and an upward start makes up for what gravity (120 voxels
		// per second squared) pulls it down meanwhile.
		const float flightSeconds = std::clamp(distance / 160.0f, 0.25f, 1.4f);
		for (int axis = 0; axis < 3; ++axis) flare.vel[axis] = toTarget[axis] / flightSeconds;
		flare.vel[2] += 0.5f * 120.0f * flightSeconds; // gravity brings it down on the target
	}
	// Into the air, burning from now on; the oldest flare goes out when there are too many.
	flare.age = 0.0f;
	flare.resting = false;
	_flares.push_back(flare);
	if (_flares.size() > FlareMax)
	{
		_flares.erase(_flares.begin());
	}
}

/**
 * Moves the flares along, once a frame. A flare flies under gravity (with heavy drag under the
 * sea), bounces off what it hits and comes to rest on a floor; while it burns it gives off red
 * fire and smoke (and a stream of bubbles under the sea), dying down in its last seconds, and
 * after FlareLife seconds (a minute) it is gone.
 * @param elapsedSeconds Time since the last frame, in seconds.
 */
void FirstPersonView::updateFlares(float elapsedSeconds)
{
	typedef FirstPersonParticles Particles;
	const float gravity = _colors.underwater ? 40.0f : 120.0f; // voxels per second squared
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY();
	for (auto &flare : _flares)
	{
		flare.age += elapsedSeconds;
		// The flight, in steps of at most 0.02 seconds, so a fast flare doesn't pass through walls.
		for (float timeLeft = elapsedSeconds; timeLeft > 0.0f && !flare.resting; timeLeft -= 0.02f)
		{
			// Gravity pulls it down; the water slows it.
			const float stepSeconds = std::min(0.02f, timeLeft);
			flare.vel[2] -= gravity * stepSeconds;
			if (_colors.underwater)
			{
				for (float &component : flare.vel) component *= std::max(0.0f, 1.0f - 2.5f * stepSeconds);
			}
			// Where this step takes it, and whether that is beyond the map's edge.
			float nextPosition[3] = { flare.pos[0] + flare.vel[0] * stepSeconds, flare.pos[1] + flare.vel[1] * stepSeconds,
				flare.pos[2] + flare.vel[2] * stepSeconds };
			float direction[3] = { nextPosition[0] - flare.pos[0], nextPosition[1] - flare.pos[1], nextPosition[2] - flare.pos[2] };
			const float stepLength = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
			const bool offMap = nextPosition[0] < 0 || nextPosition[1] < 0
				|| nextPosition[0] >= mapSizeX * 16.0f || nextPosition[1] >= mapSizeY * 16.0f;
			if (stepLength > 1e-4f && !offMap)
			{
				// Walk a ray along the step to see whether it hits anything on the way.
				for (float &component : direction) component /= stepLength;
				RayContext rayContext;
				rayContext.tiles = _tileViews.data();
				rayContext.swings = _doorSwings.data();
				rayContext.size[0] = mapSizeX; rayContext.size[1] = mapSizeY; rayContext.size[2] = _save->getMapSizeZ();
				rayContext.maxDistance = stepLength;
				Hit hit;
				float tEnd;   // where the ray left the map (not needed here)
				int endAxis;  // through which side (not needed here)
				if (castRay(rayContext, flare.pos, direction, hit, tEnd, endAxis) == RAY_HIT && hit.t < stepLength)
				{
					// It hit something: stop half a voxel short of it and bounce off, losing most
					// of the speed into the surface and half of the speed along it. Landing on a
					// floor (falling onto an upward face) slowly enough, it comes to rest.
					const int hitAxis = hit.axis >= 0 ? hit.axis : 2; // the axis across the face it hit
					for (int axis = 0; axis < 3; ++axis) flare.pos[axis] += direction[axis] * std::max(0.0f, hit.t - 0.5f);
					const bool hitFloor = hitAxis == 2 && flare.vel[2] < 0.0f;
					flare.vel[hitAxis] *= -0.3f; // bounce off it
					for (int axis = 0; axis < 3; ++axis) if (axis != hitAxis) flare.vel[axis] *= 0.5f;
					const float speed = std::sqrt(flare.vel[0] * flare.vel[0] + flare.vel[1] * flare.vel[1] + flare.vel[2] * flare.vel[2]);
					if (hitFloor && speed < 30.0f)
					{
						flare.resting = true;
					}
					continue;
				}
			}
			// Nothing in the way: take the step. Beyond the map's edge there are no voxels, only
			// the flat ground, so it lands on that; and nothing falls through the map's bottom.
			std::copy(nextPosition, nextPosition + 3, flare.pos);
			if (offMap && flare.pos[2] <= GroundTop + 0.5f)
			{
				flare.pos[2] = GroundTop + 0.5f; // the ground beyond the map edge
				flare.resting = true;
			}
			if (flare.pos[2] < 0.5f)
			{
				flare.pos[2] = 0.5f;
				flare.resting = true;
			}
		}
		// Fire, smoke and (under the sea) bubbles, dying down over the last 8 seconds. None at
		// all with particles switched off.
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		if (Options::fpsParticles == 0 || fade <= 0.0f)
		{
			continue;
		}
		// Red, orange and yellow sparks of fire licking upwards, 45 a second; the random part
		// carries the fraction of a particle over, on average.
		const int fireCount = (int)(elapsedSeconds * 45.0f * fade + _particles.random());
		for (int i = 0; i < fireCount; ++i)
		{
			Particles::Particle particle;
			std::copy(flare.pos, flare.pos + 3, particle.pos);
			particle.vel[0] = _particles.random(-4, 4); particle.vel[1] = _particles.random(-4, 4); particle.vel[2] = _particles.random(8, 20);
			particle.age = 0; particle.life = _particles.random(0.3f, 0.7f); particle.gravity = 0; particle.drag = 0.5f;
			particle.color = _colors.flareFire[(int)_particles.random(0, 2.99f)]; particle.size = 2;
			particle.kind = Particles::GLOW; particle.group = Particles::EFFECT;
			_particles.add(particle);
		}
		// A puff of reddish smoke rising, about 6 a second.
		if (_particles.random() < elapsedSeconds * 6.0f * fade)
		{
			const Uint8 smokeColors[2] = { shadeColor(_colors.flareFire[0], 6), _colors.smoke };
			// One lit particle; speed 2..6 voxels a second, mostly upwards, rising (gravity -10),
			// drag 0.6, living 1.2..2 seconds, size 3.
			_particles.burst(flare.pos, 1, Particles::LIT, Particles::EFFECT, smokeColors, 2, 2, 6, 0.9f, -10, 0.6f, 1.2f, 2.0f, 3);
		}
		// Under the sea, a stream of bubbles rising from it, 35 a second.
		if (_colors.underwater)
		{
			const int bubbleCount = (int)(elapsedSeconds * 35.0f * fade + _particles.random());
			for (int i = 0; i < bubbleCount; ++i)
			{
				Particles::Particle bubble;
				std::copy(flare.pos, flare.pos + 3, bubble.pos);
				bubble.vel[0] = _particles.random(-3, 3); bubble.vel[1] = _particles.random(-3, 3); bubble.vel[2] = _particles.random(12, 24);
				bubble.age = 0; bubble.life = _particles.random(3, 5); bubble.gravity = 0; bubble.drag = 0;
				bubble.color = _colors.bubble; bubble.size = (Uint8)(1 + (int)(_particles.random() * 3.0f));
				bubble.kind = Particles::BUBBLE; bubble.group = Particles::EFFECT;
				_particles.add(bubble);
			}
		}
	}
	// Burnt-out flares are gone.
	_flares.erase(std::remove_if(_flares.begin(), _flares.end(), [](const Flare &flare) { return flare.age > FlareLife; }), _flares.end());
}

/**
 * How much a point is lit by the flares, 0 (not at all) .. 1. A flare lights what is close to
 * it (within FlareReach voxels), facing it and, with shadows, not behind anything; its light
 * flickers slowly and fades out at the end, and matters most in the dark. Flares don't add up:
 * the strongest one that isn't hidden lights the point.
 * @param point The lit point, in voxels.
 * @param normal The surface's unit normal there, or null for a unit (lit from any side).
 * @param shadows Trace shadow rays (terrain); units are lit without them.
 * @return The flare light's strength, which the caller turns into shades and a red tint.
 */
float FirstPersonView::flareLight(const float point[3], const float normal[3], bool shadows) const
{
	const float seconds = SDL_GetTicks() / 1000.0f; // for the flicker
	const float darkness = _colors.underwater ? 0.6f : 0.3f + 0.7f * _colors.night; // a flare matters most in the dark
	// The strongest flare that isn't hidden lights the point (they don't add up): rank them
	// first, so that at most a couple of shadow rays are cast.
	float powers[FlareMax];
	float lightPositions[FlareMax][3];
	int order[FlareMax], count = 0;
	for (size_t flareIndex = 0; flareIndex < _flares.size() && count < (int)FlareMax; ++flareIndex)
	{
		// The flame is 2 voxels above where the flare lies; dx, dy, dz go from the point to it.
		const Flare &flare = _flares[flareIndex];
		const float lightPosition[3] = { flare.pos[0], flare.pos[1], flare.pos[2] + 2.0f };
		const float dx = lightPosition[0] - point[0], dy = lightPosition[1] - point[1], dz = lightPosition[2] - point[2];
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (distance > FlareReach || distance < 0.01f) continue;
		// Its strength here: falling off with the square of the distance's share of the reach,
		// fading over its last 8 seconds, and flickering (two slow waves, out of step for each
		// flare).
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		const float flicker = 0.85f + 0.075f * (std::sin(seconds * 13.0f + flareIndex * 1.7f)
			+ std::sin(seconds * 7.1f + flareIndex * 3.1f));
		float power = (1.0f - distance / FlareReach) * (1.0f - distance / FlareReach) * fade * flicker * darkness;
		// A surface turned away from it isn't lit; one facing it squarely is lit most.
		if (normal)
		{
			const float facing = (normal[0] * dx + normal[1] * dy + normal[2] * dz) / distance;
			if (facing <= 0.0f) continue;
			power *= 0.4f + 0.6f * facing;
		}
		if (power < 0.02f) continue;
		powers[count] = power;
		std::copy(lightPosition, lightPosition + 3, lightPositions[count]);
		order[count] = count;
		++count;
	}
	// Brightest first: a plain insertion sort for these few (std::sort does the same for so few,
	// but GCC 15 warns falsely about its bounds here).
	for (int sorted = 1; sorted < count; ++sorted)
	{
		for (int rank = sorted; rank > 0 && powers[order[rank - 1]] < powers[order[rank]]; --rank)
		{
			std::swap(order[rank - 1], order[rank]);
		}
	}
	// The brightest of the two strongest flares that the point can see lights it.
	for (int rank = 0; rank < count && rank < 2; ++rank)
	{
		// A faint glow casts no shadow worth a ray.
		if (!shadows || powers[order[rank]] < 0.08f || !shadowed(point, lightPositions[order[rank]]))
		{
			return powers[order[rank]];
		}
	}
	return 0.0f;
}

/**
 * Draws the flares themselves: a small red-hot glow where each one lies (a yellow core in a
 * red rim, a few pixels across depending on the distance), hidden behind what is in front of
 * it, and marked as shining for the glow around lights.
 */
void FirstPersonView::drawFlares()
{
	const int width = getWidth(), height = getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent; // pixels per voxel at one voxel's depth
	for (const auto &flare : _flares)
	{
		// Where the flare is on the view; not drawn behind the camera or right in front of it.
		const double world[3] = { flare.pos[0], flare.pos[1], flare.pos[2] + 0.5 };
		double screenX, screenY, depth;
		if (!projectToScreen(world, screenX, screenY, depth) || depth < 3.0) continue;
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		if (fade <= 0.0f) continue;
		// The glow's radius in pixels: a voxel's size on the screen there, 1 to 6 pixels.
		const int radius = std::clamp((int)std::lround(2.0 * focalLength / depth * 0.5), 1, 6);
		// Fill the disc, pixel by pixel, where nothing nearer hides it (a voxel's leeway).
		for (int y = (int)screenY - radius; y <= (int)screenY + radius; ++y)
		{
			for (int x = (int)screenX - radius; x <= (int)screenX + radius; ++x)
			{
				const double distanceFromCentre = std::sqrt((x - screenX) * (x - screenX) + (y - screenY) * (y - screenY));
				if (distanceFromCentre > radius || x < 0 || y < 0 || x >= width || y >= height
					|| depth >= _depthBuffer[y * width + x] + 1.0f) continue;
				*getRaw(x, y) = distanceFromCentre < radius * 0.5 ? _colors.flareFire[2] : _colors.flareFire[0];
				_glowMask[y * width + x] = 1;
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
