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
 * FirstPersonSounds.cpp: the few sounds the game doesn't have, made from noise and tones when
 * the add-on starts (no sound files): the hitmarker and kill ticks, the heartbeat, the click of
 * an empty gun, a flare's whoosh (also heard when the soldier catches fire), a shot whizzing
 * past and the swoosh of a melee swing. Footsteps and everything else are the game's own sounds.
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
/// Pi, for the sine waves.
const float Pi = 3.14159265f;
}

/**
 * Synthesizes all the sounds, once, for the mixer's output format (16-bit only; with any other
 * format the view simply stays silent). Each sound is first worked out as a list of mono
 * values from -1 to 1, one per sample at the mixer's sample rate, and then handed to make().
 * Making them in code means there are no sound files to ship.
 */
FirstPersonSounds::FirstPersonSounds()
{
	// Ask the mixer for its output format, and stay silent unless it is 16-bit, mono or stereo.
	int rate = 0, channels = 0;
	Uint16 format = 0;
	if (!Mix_QuerySpec(&rate, &format, &channels) || format != AUDIO_S16SYS || channels < 1 || channels > 2)
	{
		return;
	}
	// Noise comes from a generator with a fixed seed, so the sounds are the same every time.
	std::mt19937 randomEngine(7);
	std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
	// How many samples a sound lasting this many seconds takes.
	auto length = [rate](float seconds) { return (size_t)(seconds * rate); };

	// Hit: a short bright tick (60 ms), two high partials (2400 and 3700 Hz) dying away fast.
	std::vector<float> hit(length(0.06f));
	for (size_t sample = 0; sample < hit.size(); ++sample)
	{
		const float seconds = (float)sample / rate;
		hit[sample] = std::exp(-seconds * 90.0f) * (0.6f * std::sin(2 * Pi * 2400 * seconds) + 0.4f * std::sin(2 * Pi * 3700 * seconds));
	}
	make(HIT, hit);

	// Kill: the tick, then a lower one (1300 Hz with an overtone at 1950 Hz) right after, 70 ms
	// in. lowTickSeconds is the time since the lower tick began (negative before it).
	std::vector<float> kill(length(0.16f));
	for (size_t sample = 0; sample < kill.size(); ++sample)
	{
		const float seconds = (float)sample / rate, lowTickSeconds = seconds - 0.07f;
		kill[sample] = std::exp(-seconds * 80.0f) * std::sin(2 * Pi * 2400 * seconds);
		if (lowTickSeconds > 0)
		{
			kill[sample] += std::exp(-lowTickSeconds * 45.0f)
				* (0.7f * std::sin(2 * Pi * 1300 * lowTickSeconds) + 0.3f * std::sin(2 * Pi * 1950 * lowTickSeconds));
		}
	}
	make(KILL, kill);

	// Heartbeat: lub-dub, two low thumps (from about 55 and 48 Hz, each sinking in pitch as it
	// fades), the "dub" 170 ms after the "lub" and a little softer. dubSeconds is the time since
	// the "dub" began (negative before it).
	std::vector<float> beat(length(0.42f));
	for (size_t sample = 0; sample < beat.size(); ++sample)
	{
		const float seconds = (float)sample / rate, dubSeconds = seconds - 0.17f;
		float amplitude = std::exp(-seconds * 22.0f) * std::sin(2 * Pi * (55 - 15 * seconds) * seconds);
		if (dubSeconds > 0) amplitude += 0.75f * std::exp(-dubSeconds * 26.0f) * std::sin(2 * Pi * (48 - 12 * dubSeconds) * dubSeconds);
		// Fade in over the first 2.5 ms, so the sound doesn't start with a click.
		beat[sample] = amplitude * std::min(1.0f, seconds * 400.0f);
	}
	make(HEARTBEAT, beat);

	// Dry fire: a metallic click (50 ms), a burst of noise through a ringing tone (1750 Hz).
	std::vector<float> dry(length(0.05f));
	for (size_t sample = 0; sample < dry.size(); ++sample)
	{
		const float seconds = (float)sample / rate;
		dry[sample] = std::exp(-seconds * 160.0f) * (0.5f * noise(randomEngine) + 0.5f * std::sin(2 * Pi * 1750 * seconds));
	}
	make(DRY_FIRE, dry);

	// Pin: a grenade's pin pulled (140 ms): a click like the dry fire's but brighter (2600 Hz),
	// then 60 ms later the lever flying off, a softer high ping (3300 Hz) ringing a little
	// longer. leverSeconds is the time since the lever's ping began (negative before it).
	std::vector<float> pin(length(0.14f));
	for (size_t sample = 0; sample < pin.size(); ++sample)
	{
		const float seconds = (float)sample / rate, leverSeconds = seconds - 0.06f;
		pin[sample] = std::exp(-seconds * 180.0f) * (0.45f * noise(randomEngine) + 0.55f * std::sin(2 * Pi * 2600 * seconds));
		if (leverSeconds > 0) pin[sample] += 0.5f * std::exp(-leverSeconds * 60.0f) * std::sin(2 * Pi * 3300 * leverSeconds);
	}
	make(PIN, pin);

	// Light switch: the flashlight's switch (70 ms), a soft double click with no noise in it (so
	// the noise of the sounds after it stays as it was): a low knock (900 Hz with an overtone at
	// 2300 Hz), and 25 ms later the catch, quieter and higher (1500 and 3600 Hz).
	// catchSeconds is the time since the catch began (negative before it).
	std::vector<float> lightSwitch(length(0.07f));
	for (size_t sample = 0; sample < lightSwitch.size(); ++sample)
	{
		const float seconds = (float)sample / rate, catchSeconds = seconds - 0.025f;
		lightSwitch[sample] = 0.6f * std::exp(-seconds * 220.0f) * (0.7f * std::sin(2 * Pi * 900 * seconds) + 0.3f * std::sin(2 * Pi * 2300 * seconds));
		if (catchSeconds > 0)
		{
			lightSwitch[sample] += 0.35f * std::exp(-catchSeconds * 260.0f)
				* (0.6f * std::sin(2 * Pi * 1500 * catchSeconds) + 0.4f * std::sin(2 * Pi * 3600 * catchSeconds));
		}
	}
	make(LIGHT_SWITCH, lightSwitch);

	// Flare: a strike, then a hiss fading out (1.4 seconds in all).
	std::vector<float> flare(length(1.4f));
	float lowPassedNoise = 0.0f; // The noise smoothed by a simple low-pass filter.
	for (size_t sample = 0; sample < flare.size(); ++sample)
	{
		const float seconds = (float)sample / rate;
		const float whiteNoise = noise(randomEngine);
		lowPassedNoise += (whiteNoise - lowPassedNoise) * 0.35f; // A little softer than white noise.
		// The strike: a burst of plain noise, gone within a few hundredths of a second.
		const float strike = std::exp(-seconds * 60.0f) * whiteNoise;
		// The hiss: the noise minus its smoothed copy (which leaves its high, sharp part),
		// swelling over the first 80 ms and then slowly dying away.
		const float hiss = std::min(1.0f, seconds * 12.0f) * std::exp(-seconds * 1.6f) * (whiteNoise - lowPassedNoise) * 0.5f;
		flare[sample] = strike * 0.8f + hiss;
	}
	make(FLARE, flare);

	// Makes noise through a resonance sweeping from one pitch to another, in an envelope (the
	// whizz and the swoosh below). The resonance is a two-pole filter that rings at its pitch,
	// so the noise comes out as a breathy tone at that pitch. The pitch glides from fromHz to
	// toHz over durationSeconds, evenly in musical terms (by the same ratio every second);
	// the sound is loudest peakAt seconds in, and the higher the sharpness, the quicker it
	// swells and fades around that moment.
	auto sweep = [&](float durationSeconds, float fromHz, float toHz, float peakAt, float sharpness) {
		std::vector<float> sound(length(durationSeconds));
		// The filter's last two outputs.
		float lastOutput = 0.0f, secondLastOutput = 0.0f;
		// How close the filter's poles sit to the unit circle: the closer, the longer and
		// purer it rings.
		const float poleRadius = 0.985f;
		for (size_t sample = 0; sample < sound.size(); ++sample)
		{
			// Where we are: seconds since the start, and progress through the sound (0 to 1).
			const float seconds = (float)sample / rate, progress = seconds / durationSeconds;
			// The resonance's pitch now, in Hz and as an angle turned per sample.
			const float pitchHz = fromHz * std::pow(toHz / fromHz, progress);
			const float radiansPerSample = 2 * Pi * pitchHz / rate;
			// One step of the filter: new noise (scaled down, as the filter boosts its pitch a
			// lot) plus the ringing of the last two outputs.
			const float filtered = noise(randomEngine) * (1 - poleRadius) * 6.0f
				+ 2 * poleRadius * std::cos(radiansPerSample) * lastOutput - poleRadius * poleRadius * secondLastOutput;
			secondLastOutput = lastOutput;
			lastOutput = filtered;
			// A bell-shaped envelope (a Gaussian curve), loudest at peakAt.
			const float envelope = std::exp(-std::pow((seconds - peakAt) * sharpness, 2.0f));
			sound[sample] = std::clamp(filtered, -1.0f, 1.0f) * envelope;
		}
		return sound;
	};
	// A near miss: a shot whizzing past, its pitch falling as it goes (Doppler).
	make(WHIZZ, sweep(0.34f, 3200.0f, 700.0f, 0.12f, 14.0f));
	// A swing: air cut by a blade or a club, rising.
	make(SWOOSH, sweep(0.24f, 500.0f, 1700.0f, 0.13f, 16.0f));
	// All made: play() may play them from now on.
	_ready = true;
}

/**
 * Frees the sounds, stopping any that is still playing first (the samples a chunk plays from
 * go away with this object).
 */
FirstPersonSounds::~FirstPersonSounds()
{
	for (Mix_Chunk *chunk : _chunks)
	{
		if (chunk)
		{
			// Halt every channel still playing this sound (asking to allocate -1 channels just
			// tells how many there are), then free it.
			for (int channel = 0, channelCount = Mix_AllocateChannels(-1); channel < channelCount; ++channel)
			{
				if (Mix_GetChunk(channel) == chunk) Mix_HaltChannel(channel);
			}
			Mix_FreeChunk(chunk);
		}
	}
}

/**
 * Turns a mono sound into a chunk in the mixer's format: 16-bit samples, the same value on
 * every channel (so in stereo it sits in the middle until play() pans it). The chunk plays
 * straight from _samples[id] without a copy, so those samples are kept as long as the chunk.
 * @param id Which sound it is.
 * @param mono The sound, one value per sample from -1 to 1 (anything beyond is clipped).
 */
void FirstPersonSounds::make(Id id, const std::vector<float> &mono)
{
	// Ask the mixer how many channels it plays (1 for mono, 2 for stereo).
	int rate = 0, channels = 0;
	Uint16 format = 0;
	Mix_QuerySpec(&rate, &format, &channels);
	// Convert each value to a 16-bit sample, a little below full scale (26000 of 32767), and
	// write it once for each channel (the channels' samples take turns).
	std::vector<Sint16> &samples = _samples[id];
	samples.reserve(mono.size() * channels);
	for (float value : mono)
	{
		const Sint16 pcmSample = (Sint16)std::lround(std::clamp(value, -1.0f, 1.0f) * 26000.0f);
		for (int channel = 0; channel < channels; ++channel) samples.push_back(pcmSample);
	}
	// Wrap the samples in a chunk the mixer can play, without copying them (the size is in bytes).
	_chunks[id] = Mix_QuickLoad_RAW((Uint8 *)samples.data(), (Uint32)(samples.size() * sizeof(Sint16)));
}

/**
 * Plays one of the sounds once, on a free channel of the game's sound effects, so it plays at
 * the game's sound volume. With stereo sound on, it is panned towards where it comes from.
 * Nothing plays when sound is muted or the sounds couldn't be made.
 * @param id Which sound.
 * @param angle Where it comes from, in degrees: 0 ahead, 90 right (as SDL_mixer counts:
 *  180 behind, 270 left).
 * @param volume How loud, from 0 to 1, on top of the game's sound volume.
 */
void FirstPersonSounds::play(Id id, int angle, float volume)
{
	// Nothing to play: the mixer's format isn't supported, sound is muted, or this sound wasn't made.
	if (!_ready || Options::mute || !_chunks[id])
	{
		return;
	}
	// Give the sound its loudness and play it on the first free channel, without looping.
	Mix_VolumeChunk(_chunks[id], (int)std::lround(std::clamp(volume, 0.0f, 1.0f) * MIX_MAX_VOLUME));
	const int channel = Mix_PlayChannel(-1, _chunks[id], 0);
	// Pan it towards where it comes from (distance 0: as loud as it is).
	if (channel != -1 && Options::StereoSound)
	{
		Mix_SetPosition(channel, (Sint16)angle, 0);
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
