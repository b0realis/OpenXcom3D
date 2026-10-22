#pragma once
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
 * FirstPersonParticles.h: the particle system of the 3D view.
 */
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

/**
 * Particles for the first-person view: sparks, embers, smoke, debris, dust motes,
 * fireflies, rain and snow. Positions are in voxels; the view spawns and draws them.
 * This class only keeps the particles and moves them along: FirstPersonView decides when
 * and where they appear, and draws them.
 */
class FirstPersonParticles
{
public:
	/// How a particle is drawn and lit.
	enum Kind : Uint8
	{
		GLOW,    // Self-lit (sparks, embers, flashes, fireflies); darkens as it ages.
		LIT,     // Lit by the battlescape light where it is (smoke, dust, debris, snow).
		STREAK,  // Drawn as a short line along its velocity (rain, tracers).
		BLINK,   // Self-lit and blinking (fireflies).
		BUBBLE,  // Lit; drawn as a little ring when close, wobbling as it rises.
		MOTE,    // Self-lit and steady (a UFO's aura): full brightness for most of its life, fading only at the end.
		WISP,    // A wisp of the game's smoke, drawn see-through with the smoke (see FirstPersonView::drawSeeThroughSmoke).
	};
	/// Where a particle comes from, so a whole group can be counted and thinned out:
	/// EFFECT for what the battle makes (shots, impacts, explosions, fire, smoke),
	/// AMBIENT for the life around the camera (dust motes, fireflies, bubbles),
	/// WEATHER for rain and snow (and the marine snow under the sea).
	enum Group : Uint8 { EFFECT, AMBIENT, WEATHER };
	/// One particle.
	struct Particle
	{
		float pos[3], vel[3]; // Position in voxels (z points up) and velocity in voxels per second.
		float age, life;      // Seconds it has lived, and seconds it lives in all.
		float gravity;        // Pull downwards, in voxels per second squared (negative rises).
		float drag;           // Fraction of the velocity lost per second.
		Uint8 color;          // Palette colour.
		Uint8 size;           // Apparent size at one tile distance, in pixels.
		Kind kind;            // How it is drawn and lit.
		Group group;          // Where it comes from.
	};
private:
	std::vector<Particle> _particles; // The living particles.
	Uint32 _seed;                     // State of the particles' own random number generator (never zero).
	bool _accepting = true;           // New particles are taken in (not while particles are off: see setAccepting).
	static const size_t Max = 6000;   // The most particles there can be at once; more are dropped.
public:
	/// Creates an empty particle system.
	FirstPersonParticles();
	/// Removes all particles.
	void clear() { _particles.clear(); }
	/// Adds a particle (dropped when full, or while not accepting any).
	void add(const Particle &particle);
	/// Takes in new particles, or drops every one added (particles off: the battle's effects still run for what isn't a particle).
	void setAccepting(bool accepting) { _accepting = accepting; }
	/// Moves particles along and removes the expired ones; `elapsedSeconds` is the time since the last frame.
	void update(float elapsedSeconds);
	/// Removes the particles of a group farther than `radius` (horizontally) from a point, all in voxels.
	void cull(Group group, float centerX, float centerY, float radius);
	/// How many particles of a group there are.
	int count(Group group) const;
	/// All the particles.
	const std::vector<Particle> &all() const { return _particles; }
	/// Random number in [0, 1), from the particles' own generator (the game's random numbers are left alone).
	float random();
	/// Random number in [minimum, maximum).
	float random(float minimum, float maximum) { return minimum + (maximum - minimum) * random(); }
	/// Throws `particleCount` particles out of a point (`origin`, in voxels) in random directions.
	void burst(const float origin[3], int particleCount, Kind kind, Group group, const Uint8 *colors, int colorCount,
		float speedMin, float speedMax, float upBias, float gravity, float drag, float lifeMin, float lifeMax, Uint8 size);
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
