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
 * FirstPersonAmbience.h: the 3D view's ambient sounds: the rain, the wind and the fires near
 * the soldier.
 */
#include <string>
#include <vector>
#include <SDL_mixer.h>

namespace OpenXcom
{

/**
 * What the soldier hears round them this moment, as the 3D view finds it (see
 * FirstPersonView::ambientScene): the battle's weather, how much of the sky is open round the
 * eye, and the fires near it.
 */
struct AmbientScene
{
	bool ready = false;             // the view has drawn the battle (its sky and tiles looked up): until then nothing is heard
	std::string weather;            // the battle's weather, as the sky has it: "rain", "snow", "dust" or empty (none)
	bool terrainAmbience = false;   // the battle's terrain plays its own ambient loop (TFTD's sea and coasts, a big mod's town)
	float openSky = 1.0f;           // how much the eye is out under the sky: 1 in the open (or under a roof open on its sides), 0 deep indoors
	float fireLoudness = 0.0f;      // the fires near the eye together: 0.7 for one a tile away, less further, none past 9 tiles
	float fireAngle = 0.0f;         // where they are heard from, in degrees: 0 ahead, 90 right, 270 left (as SDL_mixer counts)
	float fireFocus = 0.0f;         // how much they lie to one side: 1 all one way, 0 all round the soldier
	float fireDistance = 0.0f;      // the nearest one, in tiles
	float fireThroughWalls = 1.0f;  // how much of the loudest one comes through: 1 nothing between, 0.5 a window, 0 a wall
};

/**
 * The rain, the wind and the fires near the soldier, heard in the 3D view only (setting
 * "Ambient sounds": Off, Quiet, On). Six loops, made from noise by the code below the first
 * time the 3D view shows the battle (no sound files: nothing to ship), each on a mixer channel
 * of its own: rain and wind outdoors and as heard indoors, each pair playing in step so they
 * cross-fade as the soldier walks in and out from under the roofs; and a fire near and far
 * (muffled), cross-faded by the distance, louder the nearer and panned to its side. They play
 * at the game's sound volume (its slider and mute), well under its footsteps and shots, and
 * fade out when the isometric map comes back.
 */
class FirstPersonAmbience
{
public:
	/// Takes nothing yet: the loops are made when the 3D view first shows the battle.
	FirstPersonAmbience() = default;
	/// Stops the loops, frees them and gives the mixer channels back.
	~FirstPersonAmbience();
	/// Eases the loops towards what the soldier hears now (null: the 3D view isn't showing).
	void update(const AmbientScene *scene, double elapsedSeconds);
private:
	/// The loops: rain and wind outdoors and indoors, and a fire near and far. Each pair is the
	/// same sound, sample for sample, heard two ways, so the two play in step.
	enum Loop { RAIN_OUTDOORS, RAIN_INDOORS, WIND_OUTDOORS, WIND_INDOORS, FIRE_NEAR, FIRE_FAR, LOOP_COUNT };
	/// The game's channels 0..4 are reserved (the interface, its ambience, the units' voices);
	/// ours come right after them.
	static const int FirstChannel = 5;
	bool _made = false;                    // the loops have been made (or found impossible: a mixer that isn't 16-bit)
	std::vector<Sint16> _samples[LOOP_COUNT]; // each loop in the mixer's format; its chunk plays straight from it
	Mix_Chunk *_chunks[LOOP_COUNT] = {};   // each loop as a chunk the mixer plays (null: not needed in this battle)
	float _gain[LOOP_COUNT] = {};          // each loop's loudness now, 0..1, eased towards what the scene asks
	bool _channelsTaken = false;           // our six channels have been added to the mixer's and reserved
	int _panLeft = -1, _panRight = -1;     // the fire's panning last set (-1: none yet)
	/// Makes the loops the battle needs, and starts them all, silent and in step.
	void make(const std::string &weather);
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
