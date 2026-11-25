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
 * FirstPersonParticles.cpp: the particle system of the 3D view: sparks, embers, smoke, dust,
 * rain, snow, bubbles... Each particle moves with its velocity, gravity and drag, and dies when
 * its life is over; FirstPersonView spawns and draws them.
 */
#include "FirstPersonParticles.h"
#include <algorithm>
#include <cmath>

namespace OpenXcom
{

/**
 * Creates an empty particle system. Room for the most particles there can ever be (Max) is
 * reserved up front, so adding particles in the middle of a firefight never has to grow the
 * list. The random number generator starts from a fixed seed (0x9E3779B9, the "golden ratio"
 * constant); any seed but zero works for this kind of generator.
 */
FirstPersonParticles::FirstPersonParticles() : _seed(0x9E3779B9u)
{
	_particles.reserve(Max);
}

/**
 * Adds one particle. When the system already holds its maximum (Max), the new particle is
 * quietly dropped, so a huge explosion can never slow the game down with endless particles.
 * While particles are off (see setAccepting) every one is dropped.
 * @param particle The particle to add (it is copied).
 */
void FirstPersonParticles::add(const Particle &particle)
{
	if (_accepting && _particles.size() < Max)
	{
		_particles.push_back(particle);
	}
}

/**
 * Gives the next number of the particles' own small, fast random number generator (an
 * "xorshift" generator). It is separate from the game's random numbers, so the visual effects
 * never change how the battle goes.
 * @return A random number from 0 up to, but not including, 1.
 */
float FirstPersonParticles::random()
{
	// Scramble the 32-bit state into the next one with three shifts and exclusive ors.
	_seed ^= _seed << 13;
	_seed ^= _seed >> 17;
	_seed ^= _seed << 5;
	// Keep the lowest 24 bits (as many as a float holds exactly) and scale them to [0, 1).
	return (_seed & 0xFFFFFF) / 16777216.0f;
}

/**
 * Moves every particle along by one frame and removes the ones that have lived out their
 * lifetime. Each particle ages, is pulled down by its own gravity (a negative gravity makes
 * smoke and bubbles rise), loses some of its speed to drag (air resistance) and moves on by its
 * velocity. Positions are in voxels and velocities in voxels per second.
 * A piece of a body (CHUNK), given what is solid, meets the world: it moves along one axis at a
 * time, in steps of at most a voxel (a fast piece doesn't pass through a thin wall), and a step
 * from an open voxel into a solid one is not taken. Falling onto a floor it bounces a little and
 * slides to a stop; against a wall or a ceiling it bounces back, losing most of its speed. (One
 * that starts inside something solid moves freely until it is out.)
 * @param elapsedSeconds Seconds since the last frame.
 * @param solid Whether the voxel at x, y, z is filled; none: nothing is met.
 */
void FirstPersonParticles::update(float elapsedSeconds, const std::function<bool(int, int, int)> &solid)
{
	// Age, slow down and move each particle.
	for (auto &particle : _particles)
	{
		particle.age += elapsedSeconds;
		// Drag takes away a fraction of the speed per second (never more than all of it, even
		// after a very long frame).
		const float damping = std::max(0.0f, 1.0f - particle.drag * elapsedSeconds);
		// Gravity acts on the vertical axis (z, which points up).
		particle.vel[2] -= particle.gravity * elapsedSeconds;
		for (int axis = 0; axis < 3; ++axis)
		{
			particle.vel[axis] *= damping;
		}
		if (particle.kind != CHUNK || !solid)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				particle.pos[axis] += particle.vel[axis] * elapsedSeconds;
			}
			continue;
		}
		// (the voxel a point is in)
		auto filled = [&solid](const float at[3]) { return solid((int)std::floor(at[0]), (int)std::floor(at[1]), (int)std::floor(at[2])); };
		float move[3] = { particle.vel[0] * elapsedSeconds, particle.vel[1] * elapsedSeconds, particle.vel[2] * elapsedSeconds };
		const float longest = std::max(std::abs(move[0]), std::max(std::abs(move[1]), std::abs(move[2])));
		const int steps = std::clamp((int)std::ceil(longest), 1, 8);
		for (int step = 0; step < steps; ++step)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				if (move[axis] == 0.0f) continue;
				float next[3] = { particle.pos[0], particle.pos[1], particle.pos[2] };
				next[axis] += move[axis] / steps;
				if (!filled(next) || filled(particle.pos))
				{
					particle.pos[axis] = next[axis];
					continue;
				}
				// Met: the rest of this frame's move along that axis is lost.
				move[axis] = 0.0f;
				if (axis == 2 && particle.vel[2] < 0.0f)
				{
					// On a floor: a little bounce (none once it is slow), and the slide slowed.
					particle.vel[2] = -particle.vel[2] * 0.25f;
					if (particle.vel[2] < 12.0f) particle.vel[2] = 0.0f;
					particle.vel[0] *= 0.55f;
					particle.vel[1] *= 0.55f;
				}
				else
				{
					particle.vel[axis] = -particle.vel[axis] * 0.3f;
				}
			}
		}
	}
	// Remove the expired particles (the others keep their order).
	_particles.erase(std::remove_if(_particles.begin(), _particles.end(),
		[](const Particle &particle) { return particle.age >= particle.life; }), _particles.end());
}

/**
 * Removes the particles of one group that are farther than a radius from a point, measured
 * horizontally (height doesn't count). The view calls it with the eye's position for the
 * ambient and weather particles, so the ones the soldier has walked away from make room for
 * new ones around him.
 * @param group The group to thin out (particles of other groups are always kept).
 * @param centerX The point's x, in voxels.
 * @param centerY The point's y, in voxels.
 * @param radius The distance to keep particles within, in voxels.
 */
void FirstPersonParticles::cull(Group group, float centerX, float centerY, float radius)
{
	// Squared distances are compared, which saves a square root per particle.
	const float radiusSquared = radius * radius;
	_particles.erase(std::remove_if(_particles.begin(), _particles.end(), [=](const Particle &particle)
	{
		// dx, dy: how far the particle is from the point along x and along y.
		const float dx = particle.pos[0] - centerX, dy = particle.pos[1] - centerY;
		return particle.group == group && dx * dx + dy * dy > radiusSquared;
	}), _particles.end());
}

/**
 * Counts the particles of one group, so the view knows how many ambient or weather particles
 * it has to add to reach the number it wants.
 * @param group The group to count.
 * @return How many particles of that group there are now.
 */
int FirstPersonParticles::count(Group group) const
{
	return (int)std::count_if(_particles.begin(), _particles.end(), [group](const Particle &particle) { return particle.group == group; });
}

/**
 * Throws a number of particles out of a point in random directions, like sparks and smoke
 * from an impact or an explosion. The directions are spread evenly over a sphere and then
 * tilted upwards; speed, colour and lifetime are each picked at random within their range.
 * @param origin The point they fly out of, in voxels.
 * @param particleCount How many to throw (fewer get in when the system fills up).
 * @param kind How they are drawn (see Kind).
 * @param group The group they belong to (see Group).
 * @param colors Palette colours to pick from at random, colorCount of them.
 * @param colorCount How many colours `colors` holds.
 * @param speedMin Slowest starting speed, in voxels per second.
 * @param speedMax Fastest starting speed (not included), in voxels per second.
 * @param upBias Added to the upward part of every direction: 0 throws them evenly all
 *  around, more throws more of them upwards (and those faster, as the direction isn't
 *  shortened back to length 1).
 * @param gravity Downward pull, in voxels per second squared (negative rises, like smoke).
 * @param drag Fraction of the velocity lost per second.
 * @param lifeMin Shortest lifetime, in seconds.
 * @param lifeMax Longest lifetime (not included), in seconds.
 * @param size Apparent size at one tile's distance, in pixels.
 */
void FirstPersonParticles::burst(const float origin[3], int particleCount, Kind kind, Group group, const Uint8 *colors, int colorCount,
	float speedMin, float speedMax, float upBias, float gravity, float drag, float lifeMin, float lifeMax, Uint8 size)
{
	for (int i = 0; i < particleCount; ++i)
	{
		Particle particle;
		// A random direction, spread evenly over a sphere: a random height on the unit sphere
		// (evenly from -1 to 1) and a random compass angle (a full turn is 2 pi radians), with
		// the horizontal part as long as it must be for the direction to have length 1.
		const float yaw = random(0.0f, 6.2831853f), directionZ = random(-1.0f, 1.0f);
		const float horizontalLength = std::sqrt(std::max(0.0f, 1.0f - directionZ * directionZ));
		const float speed = random(speedMin, speedMax);
		// The direction, tilted upwards by the bias (fountains of sparks, rising smoke).
		const float direction[3] = { horizontalLength * std::cos(yaw), horizontalLength * std::sin(yaw), directionZ + upBias };
		// It starts at the point, flying along the direction at its speed.
		for (int axis = 0; axis < 3; ++axis)
		{
			particle.pos[axis] = origin[axis];
			particle.vel[axis] = direction[axis] * speed;
		}
		// Newly born, with a random lifetime and colour, and the look and physics of the burst
		// (the % keeps the colour index in range).
		particle.age = 0.0f;
		particle.life = random(lifeMin, lifeMax);
		particle.gravity = gravity;
		particle.drag = drag;
		particle.color = colors[(int)(random() * colorCount) % colorCount];
		particle.size = size;
		particle.kind = kind;
		particle.group = group;
		add(particle);
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
