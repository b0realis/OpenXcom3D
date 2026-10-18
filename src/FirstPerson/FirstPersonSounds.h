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
#include <SDL_mixer.h>

namespace OpenXcom
{

/**
 * The few sounds the first-person view adds to the game's own, made in code (no sound files
 * to ship): a crisp tick for a hit, a deeper one for a kill, a heartbeat when badly hurt, the
 * dry click of a gun that can't fire, the hiss of a flare. They play through the game's sound
 * channels at its sound volume, and not at all when sound is muted.
 */
class FirstPersonSounds
{
public:
	enum Id { HIT, KILL, HEARTBEAT, DRY_FIRE, FLARE, WHIZZ, SWOOSH, COUNT };
	FirstPersonSounds();
	~FirstPersonSounds();
	/// Plays a sound, panned to an angle (0 ahead, 90 right) at a volume (0..1).
	void play(Id id, int angle = 0, float volume = 1.0f);
private:
	int _rate = 0;
	bool _ready = false;
	std::vector<Sint16> _samples[COUNT];
	Mix_Chunk *_chunks[COUNT] = {};
	void make(Id id, const std::vector<float> &mono);
};

}
