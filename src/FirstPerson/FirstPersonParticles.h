#pragma once
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
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

/**
 * Particles for the first-person view: sparks, embers, smoke, debris, dust motes,
 * fireflies, rain and snow. Positions are in voxels; the view spawns and draws them.
 */
class FirstPersonParticles
{
public:
	enum Kind : Uint8
	{
		GLOW,    // self-lit (sparks, embers, flashes, fireflies), darkens as it ages
		LIT,     // lit by the battlescape light where it is (smoke, dust, debris, snow)
		STREAK,  // drawn as a short line along its velocity (rain, tracers)
		BLINK,   // self-lit and blinking (fireflies)
		BUBBLE,  // lit; drawn as a little ring when close, wobbling as it rises
	};
	enum Group : Uint8 { EFFECT, AMBIENT, WEATHER };
	struct Particle
	{
		float pos[3], vel[3];
		float age, life;
		float gravity;    // voxels/s^2 downwards (negative rises)
		float drag;       // fraction of velocity lost per second
		Uint8 color;
		Uint8 size;       // apparent size at one tile distance, in pixels
		Kind kind;
		Group group;
	};
private:
	std::vector<Particle> _particles;
	Uint32 _seed;
	static const size_t Max = 6000;
public:
	FirstPersonParticles();
	/// Removes all particles.
	void clear() { _particles.clear(); }
	/// Adds a particle (dropped when full).
	void add(const Particle &p);
	/// Moves particles along and removes the expired ones.
	void update(float dt);
	/// Removes the particles of a group farther than `radius` (horizontally) from a point.
	void cull(Group group, float x, float y, float radius);
	/// How many particles of a group there are.
	int count(Group group) const;
	/// All the particles.
	const std::vector<Particle> &all() const { return _particles; }
	/// Random number in [0, 1).
	float random();
	/// Random number in [a, b).
	float random(float a, float b) { return a + (b - a) * random(); }
	/// Throws `count` particles out of a point.
	void burst(const float pos[3], int count, Kind kind, Group group, const Uint8 *colors, int colorCount,
		float speedMin, float speedMax, float upBias, float gravity, float drag, float lifeMin, float lifeMax, Uint8 size);
};

}
