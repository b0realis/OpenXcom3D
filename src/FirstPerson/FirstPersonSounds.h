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
 * FirstPersonSounds.h: the sounds the add-on makes itself.
 */
#include <vector>
#include <SDL_mixer.h>

namespace OpenXcom
{

/**
 * The few sounds the first-person view adds to the game's own, made in code (no sound files
 * to ship): a crisp tick for a hit, a deeper one for a kill, a heartbeat when badly hurt, the
 * dry click of a gun that can't fire, the hiss of a flare, the flashlight's switch. They play through the game's sound
 * channels at its sound volume, and not at all when sound is muted.
 */
class FirstPersonSounds
{
public:
	/// The sounds: HIT (the tick of a hit), KILL (the tick, then a lower one, for a kill),
	/// HEARTBEAT (lub-dub, when badly hurt), DRY_FIRE (the click of a gun that can't fire),
	/// FLARE (a strike and a hiss), WHIZZ (a shot whizzing past), SWOOSH (a melee swing
	/// cutting the air), PIN (a grenade's pin pulled: a click and the lever's ping),
	/// LIGHT_SWITCH (the flashlight's switch: a soft double click), SCAN_PULSE (the motion
	/// scanner's sweep going out: a falling electronic whoop and its echoes), SCAN_BLIP_FAR,
	/// SCAN_BLIP_MID and SCAN_BLIP_NEAR (the scanner finding movement: a sharp pip, higher the
	/// nearer it is). COUNT is how many there are.
	enum Id { HIT, KILL, HEARTBEAT, DRY_FIRE, FLARE, WHIZZ, SWOOSH, PIN, LIGHT_SWITCH, SCAN_PULSE, SCAN_BLIP_FAR, SCAN_BLIP_MID, SCAN_BLIP_NEAR, COUNT };
	/// Synthesizes all the sounds for the mixer's output format.
	FirstPersonSounds();
	/// Stops and frees the sounds.
	~FirstPersonSounds();
	/// Plays a sound, panned to an angle (0 ahead, 90 right) at a volume (0..1).
	void play(Id id, int angle = 0, float volume = 1.0f);
private:
	bool _ready = false;                 // True once the sounds are made (they never are when the mixer isn't 16-bit).
	std::vector<Sint16> _samples[COUNT]; // Each sound's samples in the mixer's format; its chunk plays straight from them.
	Mix_Chunk *_chunks[COUNT] = {};      // Each sound as a chunk the mixer can play (null if not made).
	/// Turns a mono sound (-1..1) into a chunk in the mixer's format.
	void make(Id id, const std::vector<float> &mono);
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
