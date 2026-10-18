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
#include "FirstPersonSounds.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "../Engine/Options.h"

namespace OpenXcom
{

namespace
{
const float Pi = 3.14159265f;
}

/**
 * Synthesizes the sounds for the mixer's output format (16-bit only; with any other format
 * the view simply stays silent).
 */
FirstPersonSounds::FirstPersonSounds()
{
	int rate = 0, channels = 0;
	Uint16 format = 0;
	if (!Mix_QuerySpec(&rate, &format, &channels) || format != AUDIO_S16SYS || channels < 1 || channels > 2)
	{
		return;
	}
	_rate = rate;
	std::mt19937 random(7);
	std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
	auto length = [rate](float seconds) { return (size_t)(seconds * rate); };

	// hit: a short bright tick, two high partials dying fast
	std::vector<float> hit(length(0.06f));
	for (size_t i = 0; i < hit.size(); ++i)
	{
		const float t = (float)i / rate;
		hit[i] = std::exp(-t * 90.0f) * (0.6f * std::sin(2 * Pi * 2400 * t) + 0.4f * std::sin(2 * Pi * 3700 * t));
	}
	make(HIT, hit);

	// kill: the tick, then a lower one right after
	std::vector<float> kill(length(0.16f));
	for (size_t i = 0; i < kill.size(); ++i)
	{
		const float t = (float)i / rate, u = t - 0.07f;
		kill[i] = std::exp(-t * 80.0f) * std::sin(2 * Pi * 2400 * t);
		if (u > 0) kill[i] += std::exp(-u * 45.0f) * (0.7f * std::sin(2 * Pi * 1300 * u) + 0.3f * std::sin(2 * Pi * 1950 * u));
	}
	make(KILL, kill);

	// heartbeat: lub-dub, two low thumps
	std::vector<float> beat(length(0.42f));
	for (size_t i = 0; i < beat.size(); ++i)
	{
		const float t = (float)i / rate, u = t - 0.17f;
		float v = std::exp(-t * 22.0f) * std::sin(2 * Pi * (55 - 15 * t) * t);
		if (u > 0) v += 0.75f * std::exp(-u * 26.0f) * std::sin(2 * Pi * (48 - 12 * u) * u);
		beat[i] = v * std::min(1.0f, t * 400.0f);
	}
	make(HEARTBEAT, beat);

	// dry fire: a metallic click, a burst of noise through a ringing tone
	std::vector<float> dry(length(0.05f));
	for (size_t i = 0; i < dry.size(); ++i)
	{
		const float t = (float)i / rate;
		dry[i] = std::exp(-t * 160.0f) * (0.5f * noise(random) + 0.5f * std::sin(2 * Pi * 1750 * t));
	}
	make(DRY_FIRE, dry);

	// flare: a strike, then a hiss fading out
	std::vector<float> flare(length(1.4f));
	float low = 0.0f;
	for (size_t i = 0; i < flare.size(); ++i)
	{
		const float t = (float)i / rate;
		const float n = noise(random);
		low += (n - low) * 0.35f; // a little softer than white noise
		const float strike = std::exp(-t * 60.0f) * n;
		const float hiss = std::min(1.0f, t * 12.0f) * std::exp(-t * 1.6f) * (n - low) * 0.5f;
		flare[i] = strike * 0.8f + hiss;
	}
	make(FLARE, flare);

	// noise through a resonance sweeping from one pitch to another, in an envelope
	auto sweep = [&](float seconds, float fromHz, float toHz, float peakAt, float sharpness) {
		std::vector<float> out(length(seconds));
		float y1 = 0.0f, y2 = 0.0f;
		const float r = 0.985f;
		for (size_t i = 0; i < out.size(); ++i)
		{
			const float t = (float)i / rate, p = t / seconds;
			const float hz = fromHz * std::pow(toHz / fromHz, p);
			const float w = 2 * Pi * hz / rate;
			const float y = noise(random) * (1 - r) * 6.0f + 2 * r * std::cos(w) * y1 - r * r * y2;
			y2 = y1;
			y1 = y;
			const float envelope = std::exp(-std::pow((t - peakAt) * sharpness, 2.0f));
			out[i] = std::clamp(y, -1.0f, 1.0f) * envelope;
		}
		return out;
	};
	// a near miss: a shot whizzing past, its pitch falling as it goes (Doppler)
	make(WHIZZ, sweep(0.34f, 3200.0f, 700.0f, 0.12f, 14.0f));
	// a swing: air cut by a blade or a club, rising
	make(SWOOSH, sweep(0.24f, 500.0f, 1700.0f, 0.13f, 16.0f));
	_ready = true;
}

/**
 * Frees the sounds (stopping any still playing first).
 */
FirstPersonSounds::~FirstPersonSounds()
{
	for (Mix_Chunk *chunk : _chunks)
	{
		if (chunk)
		{
			for (int channel = 0, count = Mix_AllocateChannels(-1); channel < count; ++channel)
			{
				if (Mix_GetChunk(channel) == chunk) Mix_HaltChannel(channel);
			}
			Mix_FreeChunk(chunk);
		}
	}
}

/**
 * Turns a mono sound (-1..1) into a chunk in the mixer's format.
 */
void FirstPersonSounds::make(Id id, const std::vector<float> &mono)
{
	int rate = 0, channels = 0;
	Uint16 format = 0;
	Mix_QuerySpec(&rate, &format, &channels);
	std::vector<Sint16> &samples = _samples[id];
	samples.reserve(mono.size() * channels);
	for (float v : mono)
	{
		const Sint16 s = (Sint16)std::lround(std::clamp(v, -1.0f, 1.0f) * 26000.0f);
		for (int c = 0; c < channels; ++c) samples.push_back(s);
	}
	_chunks[id] = Mix_QuickLoad_RAW((Uint8 *)samples.data(), (Uint32)(samples.size() * sizeof(Sint16)));
}

/**
 * Plays a sound, panned to an angle (0 ahead, 90 right) at a volume (0..1).
 */
void FirstPersonSounds::play(Id id, int angle, float volume)
{
	if (!_ready || Options::mute || !_chunks[id])
	{
		return;
	}
	Mix_VolumeChunk(_chunks[id], (int)std::lround(std::clamp(volume, 0.0f, 1.0f) * MIX_MAX_VOLUME));
	const int channel = Mix_PlayChannel(-1, _chunks[id], 0);
	if (channel != -1 && Options::StereoSound)
	{
		Mix_SetPosition(channel, (Sint16)angle, 0);
	}
}

}
