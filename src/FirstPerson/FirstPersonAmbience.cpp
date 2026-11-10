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
 * FirstPersonAmbience.cpp: the rain, the wind and the fires near the soldier in the 3D view,
 * made from noise when the 3D view first shows the battle (no sound files: every sound here is
 * worked out by the code below), and played on mixer channels of their own, eased towards what
 * the view hears round the eye (see FirstPersonView::ambientScene).
 *
 * How the loops loop: events (drops, crackles, pops) lie on a circle the length of the loop;
 * the noise beds are made one cross-fade longer and their tail is faded over their head; every
 * slow change (the gusts, the rain's drift) is a sum of whole cycles over the loop. Each pair
 * (outdoors and indoors, near and far) is one sound heard two ways, sample for sample, so the
 * two can play together and cross-fade.
 */
#include "FirstPersonAmbience.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <SDL.h>
#include "../Engine/Logger.h"
#include "../Engine/Options.h"

namespace OpenXcom
{

namespace
{
/// Pi, for the sine waves and the filters.
const float Pi = 3.14159265f;
/// The slow changes (the gusts, the rain's drift, a filter's moving pitch) are worked out
/// afresh every this many samples and held between: far too quick to hear, a sixteenth of the work.
const int ControlStep = 16;

/// A plain random source with a fixed seed (the loops come out the same every time): values
/// from -1 to 1, from 0 to 1, between two values, and log-evenly between two (pitches, loudness).
struct Random
{
	std::mt19937 engine;
	explicit Random(unsigned seed) : engine(seed) {}
	float uni() { return std::uniform_real_distribution<float>(-1.0f, 1.0f)(engine); }
	float unit() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(engine); }
	float between(float low, float high) { return low + (high - low) * unit(); }
	float logBetween(float low, float high) { return low * std::pow(high / low, unit()); }
};

/// The state-variable filter of Zavalishin (topology-preserving): low-pass, band-pass and
/// high-pass outputs from one step, stable even while its pitch moves. The band output peaks
/// at Q, so a narrow band rings louder than a wide one.
struct Svf
{
	float a1 = 1, a2 = 0, a3 = 0, k = 1;    // the coefficients for the pitch and Q set by tune()
	float ic1 = 0, ic2 = 0;                 // the filter's two states
	float low = 0, band = 0, high = 0;      // the outputs of the last step
	/// Sets the pitch (Hz) and Q at a sample rate (kept below 0.45 of it).
	void tune(float hz, float q, int rate)
	{
		const float g = std::tan(Pi * std::min(hz, 0.45f * rate) / rate);
		k = 1.0f / q;
		a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
	}
	/// One step with an input sample.
	void run(float x)
	{
		const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
		low = v2; band = v1; high = x - k * v1 - v2;
	}
};

/// A one-pole low-pass (and the high-pass that is what it leaves).
struct OnePole
{
	float y = 0, a = 1;
	void set(float hz, int rate) { a = 1.0f - std::exp(-2 * Pi * hz / rate); }
	float low(float x) { y += a * (x - y); return y; }
	float high(float x) { return x - low(x); }
};

/// A smooth wandering value, mostly within -1.5..1.5, that repeats exactly every loopSeconds:
/// whole cycles over the loop (minCycle..maxCycle of them), each softer by 1/cycle^tilt, at
/// random phases.
struct Wander
{
	std::vector<float> amplitude, phase;
	float loopSeconds;
	Wander(Random &random, float loopLength, int minCycle, int maxCycle, float tilt) : loopSeconds(loopLength)
	{
		float sum = 0;
		for (int cycle = 0; cycle <= maxCycle; ++cycle)
		{
			const float a = cycle >= minCycle ? 1.0f / std::pow((float)cycle, tilt) : 0.0f;
			amplitude.push_back(a); phase.push_back(random.between(0, 2 * Pi)); sum += a * a;
		}
		for (float &a : amplitude) a /= std::sqrt(sum * 0.5f); // (an RMS of 1)
	}
	float at(float seconds) const
	{
		float value = 0;
		for (size_t cycle = 1; cycle < amplitude.size(); ++cycle)
		{
			if (amplitude[cycle] > 0) value += amplitude[cycle] * std::sin(2 * Pi * cycle * seconds / loopSeconds + phase[cycle]);
		}
		return value;
	}
};

/// Two channels of a sound, as floats (-1..1).
struct Stereo
{
	std::vector<float> left, right;
	explicit Stereo(size_t length = 0) : left(length, 0.0f), right(length, 0.0f) {}
};

/// Folds a sound one cross-fade longer than the loop into a seamless loop: the tail past the
/// loop's end fades out over the head as the head fades in (at equal power, the two being
/// unlike noise), so the last sample runs straight on into the first.
std::vector<float> fold(const std::vector<float> &longer, size_t loopLength)
{
	const size_t fade = longer.size() - loopLength;
	std::vector<float> loop(longer.begin(), longer.begin() + loopLength);
	for (size_t i = 0; i < fade; ++i)
	{
		const float t = (i + 0.5f) / fade;
		loop[i] = longer[i] * std::sin(0.5f * Pi * t) + longer[loopLength + i] * std::cos(0.5f * Pi * t);
	}
	return loop;
}

/// Adds an event's sound into a loop at a moment, running on round the loop's end.
void addWrapped(std::vector<float> &loop, size_t start, const std::vector<float> &sound, float gain)
{
	for (size_t i = 0; i < sound.size(); ++i) loop[(start + i) % loop.size()] += sound[i] * gain;
}

/// Pans a value into a left and right pair, at -1 (left) .. 1 (right), at equal power.
void pan(float value, float position, float &left, float &right)
{
	const float angle = (position + 1.0f) * 0.25f * Pi;
	left += value * std::cos(angle); right += value * std::sin(angle);
}

/// The A-weighted level of a sound in dB (an RMS of 1 is 0 dB), both channels together if
/// there are two: the ear's curve (four first-order high-passes at 20.6, 20.6, 107.7 and 737.9
/// Hz, two low-passes at 12.2 kHz), made 0 dB at 1 kHz, so that loops as unlike as a dark
/// rumble and a bright hiss are set to the same loudness to the ear.
float aWeightedDb(const std::vector<float> &first, const std::vector<float> *second, int rate)
{
	const size_t settle = rate / 10; // the filters' first tenth of a second is left out
	auto weigh = [&](const std::vector<float> &sound) {
		OnePole h1, h2, h3, h4, l1, l2;
		h1.set(20.6f, rate); h2.set(20.6f, rate); h3.set(107.7f, rate); h4.set(737.9f, rate);
		l1.set(std::min(12194.0f, 0.45f * rate), rate); l2.set(std::min(12194.0f, 0.45f * rate), rate);
		double sum = 0;
		for (size_t i = 0; i < sound.size(); ++i)
		{
			const float y = l2.low(l1.low(h4.high(h3.high(h2.high(h1.high(sound[i]))))));
			if (i >= settle) sum += (double)y * y;
		}
		return sum / std::max<size_t>(1, sound.size() - settle);
	};
	// The curve's own gain at 1 kHz, measured the same way (a sine's mean square is a half).
	std::vector<float> tone(rate);
	for (size_t i = 0; i < tone.size(); ++i) tone[i] = std::sin(2 * Pi * 1000.0f * i / rate);
	const double toneGain = weigh(tone) / 0.5;
	const double meanSquare = second ? 0.5 * (weigh(first) + weigh(*second)) : weigh(first);
	return (float)(10 * std::log10(meanSquare / toneGain + 1e-20));
}

/// Scales a sound (one or both channels) to an A-weighted level, in dB.
void levelTo(std::vector<float> &first, std::vector<float> *second, float levelDb, int rate)
{
	const float scale = std::pow(10.0f, (levelDb - aWeightedDb(first, second, rate)) / 20.0f);
	for (float &value : first) value *= scale;
	if (second) for (float &value : *second) value *= scale;
}

/// Holds a loop's peaks under a ceiling (dB) without touching the rest: a gain that dips at
/// once round each peak above it (looking a millisecond either side) and recovers over 30 ms,
/// worked out round the loop, as it wraps. A fire's loudest pops would otherwise jump out.
void limitPeaks(std::vector<float> &sound, float ceilingDb, int rate)
{
	const float ceiling = std::pow(10.0f, ceilingDb / 20.0f);
	const size_t length = sound.size(), reach = (size_t)(0.001f * rate);
	std::vector<float> gain(length, 1.0f);
	for (size_t i = 0; i < length; ++i)
	{
		if (std::abs(sound[i]) <= ceiling) continue;
		for (size_t k = 0; k <= 2 * reach; ++k)
		{
			float &g = gain[(i + length - reach + k) % length];
			g = std::min(g, ceiling / std::abs(sound[i]));
		}
	}
	// The recovery, twice round the loop (so its start knows how its end left the gain).
	const float recover = 1.0f - std::exp(-1.0f / (0.03f * rate));
	float g = 1.0f;
	for (int pass = 0; pass < 2; ++pass)
	{
		for (size_t i = 0; i < length; ++i)
		{
			g = std::min(gain[i], g + (1.0f - g) * recover);
			if (pass) gain[i] = g;
		}
	}
	for (size_t i = 0; i < length; ++i) sound[i] *= gain[i];
}

/**
 * Rain (16 seconds, stereo). Outdoors: a steady hiss of far rain (noise from 320 Hz to 6 kHz,
 * with the duller splash of drops on the ground, its level drifting a little), the sizzle of
 * countless near drops (a sparse train of sharp impulses rung through a bright band), pats of
 * drops on leaves and the ground (short noise bursts rung at 0.7-3 kHz, one in fourteen close
 * by and louder) and now and then a drop into a puddle (a tiny rising whistle: the bubble it
 * traps). Under a roof: all that muffled through the roof (low-passed near 700 Hz), the
 * drumming of the same drops on the roof (rung low) and a drip close by every four seconds.
 * The outdoor loop at -36 dB (A-weighted), the indoor one 12 dB under it, at -48.
 */
void makeRain(int rate, Stereo &outdoors, Stereo &indoors)
{
	const float loopSeconds = 16.0f;
	Random random(11);
	const size_t loopLength = (size_t)(loopSeconds * rate), fade = (size_t)(0.75f * rate), longer = loopLength + fade;
	// The noise beds and the drops' sizzle, made one cross-fade longer and folded.
	Stereo bedOut(longer), bedIn(longer);
	Wander drift(random, loopSeconds, 1, 6, 1.0f);
	Svf hissHigh[2], hissLow[2], splash[2], sizzle[2], roofDrum[2], muffleA[2], muffleB[2];
	for (int c = 0; c < 2; ++c)
	{
		hissHigh[c].tune(320, 0.7f, rate); hissLow[c].tune(6000, 0.7f, rate); splash[c].tune(380, 0.7f, rate);
		sizzle[c].tune(2900, 0.7f, rate); roofDrum[c].tune(260, 0.8f, rate);
		muffleA[c].tune(700, 0.7f, rate); muffleB[c].tune(900, 0.6f, rate);
	}
	float level = 1.0f;
	for (size_t i = 0; i < longer; ++i)
	{
		if (i % ControlStep == 0) level = 1.0f + 0.12f * drift.at((float)i / rate);
		for (int c = 0; c < 2; ++c)
		{
			// Far rain: plain noise between 320 Hz and 6 kHz, and the splash on the ground.
			const float white = random.uni();
			hissHigh[c].run(white);
			hissLow[c].run(hissHigh[c].high);
			splash[c].run(white);
			// Near drops: an impulse in about one sample of a hundred, heavy-tailed in size.
			float impulse = 0;
			if (random.unit() < 0.01f)
			{
				const float u = random.unit();
				impulse = (random.uni() < 0 ? -1 : 1) * u * u * u;
			}
			sizzle[c].run(impulse);
			// Indoors the same drops drum on the roof, rung low and wide.
			roofDrum[c].run(impulse);
			const float outdoor = level * (0.17f * hissLow[c].low + 0.12f * splash[c].band + 0.85f * sizzle[c].band);
			// Through the roof: the outdoor sound low-passed twice, and the drumming.
			muffleA[c].run(outdoor); muffleB[c].run(muffleA[c].low);
			(c ? bedOut.right : bedOut.left)[i] = outdoor;
			(c ? bedIn.right : bedIn.left)[i] = 0.75f * muffleB[c].low + 0.9f * roofDrum[c].band;
		}
	}
	outdoors.left = fold(bedOut.left, loopLength); outdoors.right = fold(bedOut.right, loopLength);
	indoors.left = fold(bedIn.left, loopLength); indoors.right = fold(bedIn.right, loopLength);
	// Pats on leaves and the ground: about 24 a second, each a few milliseconds of noise rung
	// at 0.7-3 kHz, anywhere from left to right. Indoors they are lost in the roof's sound.
	const int pats = (int)(24 * loopSeconds);
	for (int p = 0; p < pats; ++p)
	{
		const size_t start = (size_t)(random.unit() * loopLength);
		const bool close = random.unit() < 0.07f;
		const float hz = random.logBetween(700, 3000), gain = random.logBetween(0.08f, 0.7f) * (close ? 2.0f : 1.0f), where = random.uni() * 0.85f;
		const float decaySeconds = random.between(0.0008f, 0.002f);
		std::vector<float> burst((size_t)(0.012f * rate));
		Svf ring;
		ring.tune(hz, 1.6f, rate);
		for (size_t i = 0; i < burst.size(); ++i)
		{
			ring.run(random.uni() * std::exp(-(float)i / rate / decaySeconds));
			burst[i] = ring.band;
		}
		float left = 0, right = 0;
		pan(1.0f, where, left, right);
		addWrapped(outdoors.left, start, burst, gain * left); addWrapped(outdoors.right, start, burst, gain * right);
	}
	// A drop's bubble: a whistle rising from its pitch by `rise` over its decay, swelling in
	// over a third of a millisecond and dying away.
	auto plink = [&](float hz, float decaySeconds, float rise) {
		std::vector<float> sound((size_t)(decaySeconds * 6 * rate));
		float phase = 0;
		for (size_t i = 0; i < sound.size(); ++i)
		{
			const float t = (float)i / rate;
			phase += 2 * Pi * hz * (1.0f + rise * t / decaySeconds) / rate;
			sound[i] = std::min(1.0f, t * 3000.0f) * std::exp(-t / decaySeconds) * std::sin(phase);
		}
		return sound;
	};
	// Drops into puddles: about three a second, 1.1-3 kHz, dying within 5-14 ms.
	const int plinks = (int)(3 * loopSeconds);
	for (int p = 0; p < plinks; ++p)
	{
		const size_t start = (size_t)(random.unit() * loopLength);
		const std::vector<float> sound = plink(random.logBetween(1100, 3000), random.between(0.005f, 0.014f), random.between(0.3f, 0.8f));
		const float gain = random.logBetween(0.03f, 0.13f), where = random.uni() * 0.8f;
		float left = 0, right = 0;
		pan(1.0f, where, left, right);
		addWrapped(outdoors.left, start, sound, gain * left); addWrapped(outdoors.right, start, sound, gain * right);
	}
	// Indoors, a drip close by every four seconds or so, longer and softer.
	const int drips = std::max(1, (int)(loopSeconds / 4.0f));
	for (int p = 0; p < drips; ++p)
	{
		const size_t start = (size_t)((p + random.between(0.2f, 0.8f)) / drips * loopLength);
		const std::vector<float> sound = plink(random.logBetween(1300, 2200), random.between(0.018f, 0.03f), 0.35f);
		const float gain = random.logBetween(0.05f, 0.09f), where = random.uni() * 0.5f;
		float left = 0, right = 0;
		pan(1.0f, where, left, right);
		addWrapped(indoors.left, start, sound, gain * left); addWrapped(indoors.right, start, sound, gain * right);
	}
	levelTo(outdoors.left, &outdoors.right, -36.0f, rate);
	levelTo(indoors.left, &indoors.right, -48.0f, rate);
}

/**
 * Wind (24 seconds, stereo). Outdoors: slow gusts (a wandering strength, cycles of 2.4 to 24
 * seconds), and with them a low rumble, a breathy body (noise rung at 170-350 Hz, its pitch
 * rising as it blows harder, its top rolled off above 1.2 kHz), a faint whistle that sings only
 * in the strongest gusts, and the rustle of leaves or sand; the right ear hears each gust a
 * third of a second after the left, as if it crossed the field. Indoors: the rumble and the
 * body muffled (below 300 Hz), and the faint whistle of the gaps round doors and windows.
 * Outdoors at -37 dB (A-weighted) on average, indoors 12 dB under it, at -49.
 */
void makeWind(int rate, Stereo &outdoors, Stereo &indoors)
{
	const float loopSeconds = 24.0f;
	Random random(23);
	const size_t loopLength = (size_t)(loopSeconds * rate), fade = (size_t)(1.0f * rate), longer = loopLength + fade;
	Wander gusts(random, loopSeconds, 1, 10, 0.9f), whistleDrift(random, loopSeconds, 2, 5, 1.0f);
	Stereo bedOut(longer), bedIn(longer);
	Svf rumble[2], body[2], whistle[2], rustle[2], inRumble[2], inBody[2], gap[2];
	OnePole brown[2], bodyTop[2];
	float strength[2] = {};
	for (int c = 0; c < 2; ++c)
	{
		brown[c].set(400, rate); bodyTop[c].set(1200, rate);
		rustle[c].tune(3200, 0.7f, rate); inBody[c].tune(280, 0.7f, rate);
	}
	for (size_t i = 0; i < longer; ++i)
	{
		const float seconds = (float)i / rate;
		for (int c = 0; c < 2; ++c)
		{
			// The gust's strength now (0..1, the right ear 0.35 s behind the left), and the
			// filters' pitches with it.
			if (i % ControlStep == 0)
			{
				const float g = strength[c] = std::clamp(0.5f + 0.22f * gusts.at(seconds - (c ? 0.35f : 0.0f)), 0.05f, 1.0f);
				rumble[c].tune(70 + 70 * g, 0.7f, rate);
				body[c].tune(170 + 180 * g, 0.7f, rate);
				whistle[c].tune((430 + 420 * g) * (1.0f + 0.04f * whistleDrift.at(seconds) + (c ? 0.03f : 0.0f)), 16.0f, rate);
				inRumble[c].tune(55 + 45 * g, 0.7f, rate);
				gap[c].tune((650 + 300 * g) * (c ? 1.05f : 1.0f), 28.0f, rate);
			}
			const float g = strength[c];
			const float noise = random.uni(), dark = brown[c].low(noise) * 3.0f;
			rumble[c].run(dark);
			body[c].run(noise);
			whistle[c].run(noise);
			rustle[c].run(noise);
			const float outdoor = (0.35f + 0.65f * g) * 0.18f * rumble[c].low + (0.2f + 0.8f * g * g) * 0.9f * bodyTop[c].low(body[c].band)
				+ std::pow(g, 4.0f) * 0.4f * whistle[c].band / 16.0f + g * g * 0.012f * rustle[c].high;
			inRumble[c].run(dark);
			inBody[c].run(body[c].band);
			gap[c].run(noise);
			const float indoor = (0.4f + 0.6f * g) * 0.15f * inRumble[c].low + (0.15f + 0.85f * g * g) * 1.2f * inBody[c].low
				+ std::pow(g, 4.0f) * 0.5f * gap[c].band / 28.0f;
			(c ? bedOut.right : bedOut.left)[i] = outdoor;
			(c ? bedIn.right : bedIn.left)[i] = indoor;
		}
	}
	outdoors.left = fold(bedOut.left, loopLength); outdoors.right = fold(bedOut.right, loopLength);
	indoors.left = fold(bedIn.left, loopLength); indoors.right = fold(bedIn.right, loopLength);
	levelTo(outdoors.left, &outdoors.right, -37.0f, rate);
	levelTo(indoors.left, &indoors.right, -49.0f, rate);
}

/**
 * Fire (15 seconds, one channel: the game pans it to the fire's side). A low roar (dark noise
 * rung at 230-390 Hz, flickering a few times a second), a faint hiss of the flames, and the
 * crackle of burning wood: little clusters of sharp ticks rung at 1.5-6 kHz, about nine a
 * second, thicker in spells, and about one pop every two seconds, rung lower, with a soft
 * thump. Far: the same with its highs lost to the distance (low-passed at 1.1 kHz) and fewer
 * crackles. Both at -31 dB (A-weighted), the loudest pops held under -10 dB; the game plays
 * the fire at 0.7 of that a tile away (-34), less further off.
 */
void makeFire(int rate, std::vector<float> &nearFire, std::vector<float> &farFire)
{
	const float loopSeconds = 15.0f;
	Random random(37);
	const size_t loopLength = (size_t)(loopSeconds * rate), fade = (size_t)(0.5f * rate), longer = loopLength + fade;
	std::vector<float> roarBed(longer);
	OnePole flickerSmooth, dark;
	flickerSmooth.set(5.0f, rate); dark.set(900.0f, rate);
	Svf roar, hiss;
	hiss.tune(2800, 0.7f, rate);
	Wander spells(random, loopSeconds, 1, 7, 0.8f);
	for (size_t i = 0; i < longer; ++i)
	{
		const float noise = random.uni();
		const float flicker = std::clamp(0.6f + 4.0f * flickerSmooth.low(random.uni()), 0.15f, 1.4f);
		if (i % ControlStep == 0) roar.tune(230 + 160 * flicker, 0.8f, rate);
		roar.run(dark.low(noise) * 1.5f);
		hiss.run(noise);
		roarBed[i] = 0.26f * flicker * roar.band + 0.01f * flicker * hiss.high;
	}
	nearFire = fold(roarBed, loopLength);
	// The crackles: each 1-4 ticks within 12 ms, each tick a noise burst of a fraction of a
	// millisecond, the spells (a wandering rate) thickening them.
	std::vector<float> crackles(loopLength, 0.0f);
	for (float t = 0; t < loopSeconds;)
	{
		const float perSecond = 4.0f + 12.0f * std::pow(std::clamp(0.5f + 0.4f * spells.at(t), 0.0f, 1.2f), 2.0f);
		t += -std::log(1.0f - random.unit() * 0.999f) / perSecond;
		if (t >= loopSeconds) break;
		const int ticks = 1 + (int)(random.unit() * random.unit() * 4);
		const float gain = random.logBetween(0.06f, 0.9f), hz = random.logBetween(1500, 6000);
		for (int k = 0; k < ticks; ++k)
		{
			const size_t start = (size_t)((t + random.between(0, 0.012f)) * rate);
			const float decaySeconds = random.between(0.00005f, 0.0003f), tickGain = gain * random.between(0.4f, 1.0f);
			std::vector<float> tick((size_t)(0.006f * rate));
			Svf ring;
			ring.tune(hz * random.between(0.8f, 1.25f), 2.5f, rate);
			for (size_t i = 0; i < tick.size(); ++i)
			{
				ring.run(random.uni() * std::exp(-(float)i / rate / decaySeconds));
				tick[i] = ring.band + 0.3f * ring.high;
			}
			addWrapped(crackles, start, tick, tickGain);
		}
	}
	// The pops: louder, rung at 600-1500 Hz, with a soft thump at 95 Hz.
	const int pops = (int)(0.5f * loopSeconds);
	for (int p = 0; p < pops; ++p)
	{
		const size_t start = (size_t)(random.unit() * loopLength);
		const float hz = random.logBetween(600, 1500), gain = random.logBetween(0.25f, 0.7f);
		std::vector<float> pop((size_t)(0.04f * rate));
		Svf ring;
		ring.tune(hz, 2.0f, rate);
		for (size_t i = 0; i < pop.size(); ++i)
		{
			const float t = (float)i / rate;
			ring.run(random.uni() * std::exp(-t / 0.0015f));
			pop[i] = ring.band + 0.35f * std::exp(-t / 0.02f) * std::sin(2 * Pi * 95 * t) * std::min(1.0f, t * 2000.0f);
		}
		addWrapped(crackles, start, pop, gain);
	}
	for (size_t i = 0; i < loopLength; ++i) nearFire[i] += crackles[i];
	// Far: low-passed at 1.1 kHz, twice round the loop (so the filters have settled where it
	// starts), with half the crackles.
	farFire.assign(loopLength, 0.0f);
	Svf muffleA, muffleB;
	muffleA.tune(1100, 0.7f, rate); muffleB.tune(1500, 0.6f, rate);
	for (int pass = 0; pass < 2; ++pass)
	{
		for (size_t i = 0; i < loopLength; ++i)
		{
			muffleA.run(nearFire[i] - 0.5f * crackles[i]); muffleB.run(muffleA.low);
			farFire[i] = muffleB.low;
		}
	}
	for (std::vector<float> *sound : { &nearFire, &farFire })
	{
		levelTo(*sound, nullptr, -31.0f, rate);
		limitPeaks(*sound, -10.0f, rate);
		levelTo(*sound, nullptr, -31.0f, rate);
	}
}

/// A loop in the mixer's format: 16-bit, one value per channel in turn (a mono loop on both
/// channels, a stereo one mixed down for a mono mixer).
std::vector<Sint16> toMixer(const std::vector<float> &left, const std::vector<float> *right, int channels)
{
	std::vector<Sint16> samples;
	samples.reserve(left.size() * channels);
	auto pcm = [](float value) { return (Sint16)std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f); };
	for (size_t i = 0; i < left.size(); ++i)
	{
		const float l = left[i], r = right ? (*right)[i] : left[i];
		if (channels == 1)
		{
			samples.push_back(pcm(0.5f * (l + r)));
		}
		else
		{
			samples.push_back(pcm(l));
			samples.push_back(pcm(r));
		}
	}
	return samples;
}
}

/**
 * Makes the loops this battle needs, at the mixer's rate, the first time the 3D view shows it:
 * rain (and the wind under it) in a rain battle, wind in snow and dust, and the fire always
 * (any battle may burn). Then takes six mixer channels for them: the game has 16 and reserves
 * the first five; we add six and reserve eleven, so ours are 5..10 and the game keeps the
 * eleven free channels it had for its sound effects. Nothing is made, and nothing plays, with a
 * mixer that isn't 16-bit.
 * @param weather The battle's weather: "rain", "snow", "dust" or empty.
 */
void FirstPersonAmbience::make(const std::string &weather)
{
	_made = true;
	int rate = 0, channels = 0;
	Uint16 format = 0;
	if (!Mix_QuerySpec(&rate, &format, &channels) || format != AUDIO_S16SYS || channels < 1 || channels > 2)
	{
		return;
	}
	const Uint32 startTicks = SDL_GetTicks();
	if (weather == "rain")
	{
		Stereo outdoors, indoors;
		makeRain(rate, outdoors, indoors);
		_samples[RAIN_OUTDOORS] = toMixer(outdoors.left, &outdoors.right, channels);
		_samples[RAIN_INDOORS] = toMixer(indoors.left, &indoors.right, channels);
	}
	if (weather == "rain" || weather == "snow" || weather == "dust")
	{
		Stereo outdoors, indoors;
		makeWind(rate, outdoors, indoors);
		_samples[WIND_OUTDOORS] = toMixer(outdoors.left, &outdoors.right, channels);
		_samples[WIND_INDOORS] = toMixer(indoors.left, &indoors.right, channels);
	}
	std::vector<float> nearFire, farFire;
	makeFire(rate, nearFire, farFire);
	_samples[FIRE_NEAR] = toMixer(nearFire, nullptr, channels);
	_samples[FIRE_FAR] = toMixer(farFire, nullptr, channels);
	// Each loop as a chunk the mixer plays straight from its samples (the size is in bytes),
	// silent until the scene asks for it.
	for (int loop = 0; loop < LOOP_COUNT; ++loop)
	{
		if (_samples[loop].empty()) continue;
		_chunks[loop] = Mix_QuickLoad_RAW((Uint8 *)_samples[loop].data(), (Uint32)(_samples[loop].size() * sizeof(Sint16)));
		if (_chunks[loop]) Mix_VolumeChunk(_chunks[loop], 0);
	}
	// Our channels, after the game's five reserved ones. The new channels at the end (the
	// game's now) start at full volume: they take the game's sound volume, as its others have.
	const int channelCount = Mix_AllocateChannels(-1), soundVolume = Mix_Volume(FirstChannel, -1);
	Mix_AllocateChannels(channelCount + LOOP_COUNT);
	for (int channel = channelCount; channel < channelCount + LOOP_COUNT; ++channel) Mix_Volume(channel, soundVolume);
	Mix_ReserveChannels(FirstChannel + LOOP_COUNT);
	_channelsTaken = true;
	Log(LOG_INFO) << "OpenXcom3D: ambient sounds (" << (weather.empty() ? "no weather" : weather) << ") made in " << SDL_GetTicks() - startTicks << " ms";
}

/**
 * Stops the loops, frees them and gives the six channels back.
 */
FirstPersonAmbience::~FirstPersonAmbience()
{
	if (_channelsTaken)
	{
		for (int loop = 0; loop < LOOP_COUNT; ++loop) Mix_HaltChannel(FirstChannel + loop);
		Mix_ReserveChannels(FirstChannel);
		Mix_AllocateChannels(Mix_AllocateChannels(-1) - LOOP_COUNT);
	}
	for (Mix_Chunk *chunk : _chunks)
	{
		if (chunk) Mix_FreeChunk(chunk);
	}
}

/**
 * Eases each loop's loudness towards what the soldier hears now, over about a third of a
 * second (the fire's side at once):
 * - the rain in a rain battle, the wind in snow and dust (and a step lower under the rain),
 *   outdoors and indoors cross-faded by how open the sky is round the eye (at equal power,
 *   the outdoor loop as loud as the sky is open); both halved when
 *   the battle's terrain plays an ambient loop of its own, to share the room with it;
 * - the fires: near and far (muffled) cross-faded by the distance, from 2 tiles to 7, and by
 *   what is between (a wall: all far, and half as loud), panned to their side (the nearer ear
 *   full, the other down to 0.3 for a fire straight to one side);
 * - all of it at the setting's level (Quiet: half, -6 dB), and nothing with the 3D view gone.
 * The loops start together, silent and in step, the first time the 3D view shows the battle
 * with the setting on, and keep playing (silent ones cost the mixer nothing) until the battle
 * ends.
 * @param scene What the soldier hears round them, or null when the 3D view isn't showing.
 * @param elapsedSeconds Seconds since the last update.
 */
void FirstPersonAmbience::update(const AmbientScene *scene, double elapsedSeconds)
{
	const float level = Options::fpsAmbientSounds >= 2 ? 1.0f : Options::fpsAmbientSounds == 1 ? 0.5f : 0.0f;
	if (Options::mute)
	{
		return;
	}
	if (!_made)
	{
		// Made the first time the 3D view shows the battle with the setting on (the weather is
		// known by then: the view has looked up its sky).
		if (!scene || level <= 0.0f)
		{
			return;
		}
		make(scene->weather);
	}
	if (!_channelsTaken)
	{
		return;
	}
	// Start (or restart: something halted every channel) the loops together, in step; only
	// once our channels are quiet, so no sound the game was still playing on them is cut.
	bool allPlaying = true, anyBusy = false;
	for (int loop = 0; loop < LOOP_COUNT; ++loop)
	{
		const int channel = FirstChannel + loop;
		if (_chunks[loop] && (!Mix_Playing(channel) || Mix_GetChunk(channel) != _chunks[loop])) allPlaying = false;
		if (Mix_Playing(channel) && Mix_GetChunk(channel) != _chunks[loop]) anyBusy = true;
	}
	if (!allPlaying)
	{
		if (anyBusy)
		{
			return;
		}
		SDL_LockAudio();
		for (int loop = 0; loop < LOOP_COUNT; ++loop)
		{
			if (_chunks[loop]) Mix_PlayChannel(FirstChannel + loop, _chunks[loop], -1);
		}
		SDL_UnlockAudio();
		_panLeft = _panRight = -1;
	}

	// What each loop should be at now.
	float target[LOOP_COUNT] = {};
	if (scene)
	{
		const bool rain = scene->weather == "rain", windy = rain || scene->weather == "snow" || scene->weather == "dust";
		const float share = scene->terrainAmbience ? 0.5f : 1.0f;
		// Outdoors as much as the sky is open, indoors the rest of the power (a small gap lets in
		// little of the outdoor sound).
		const float outdoors = std::clamp(scene->openSky, 0.0f, 1.0f), indoors = std::sqrt(1.0f - outdoors * outdoors);
		if (rain)
		{
			target[RAIN_OUTDOORS] = outdoors * share;
			target[RAIN_INDOORS] = indoors * share;
		}
		if (windy)
		{
			const float wind = (rain ? 0.5f : 1.0f) * share;
			target[WIND_OUTDOORS] = outdoors * wind;
			target[WIND_INDOORS] = indoors * wind;
		}
		const float through = std::clamp(scene->fireThroughWalls, 0.0f, 1.0f);
		const float farShare = std::max(std::clamp((scene->fireDistance - 2.0f) / 5.0f, 0.0f, 1.0f), 1.0f - through);
		const float fire = std::min(1.0f, scene->fireLoudness) * (0.5f + 0.5f * through);
		target[FIRE_NEAR] = fire * std::sqrt(1.0f - farShare);
		target[FIRE_FAR] = fire * std::sqrt(farShare);
	}
	const float ease = 1.0f - (float)std::exp(-elapsedSeconds / 0.3);
	for (int loop = 0; loop < LOOP_COUNT; ++loop)
	{
		if (!_chunks[loop]) continue;
		const float wanted = target[loop] * level;
		_gain[loop] += (wanted - _gain[loop]) * ease;
		if (wanted == 0.0f && _gain[loop] < 0.004f) _gain[loop] = 0.0f;
		Mix_VolumeChunk(_chunks[loop], (int)std::lround(std::clamp(_gain[loop], 0.0f, 1.0f) * MIX_MAX_VOLUME));
	}
	// The fire to its side, as much as the fires lie to one side.
	if (scene && Options::StereoSound && _chunks[FIRE_NEAR])
	{
		const float side = 0.7f * std::clamp(scene->fireFocus, 0.0f, 1.0f) * std::sin(scene->fireAngle * Pi / 180.0f);
		const int left = (int)std::lround(255 * (1.0f - std::max(side, 0.0f))), right = (int)std::lround(255 * (1.0f + std::min(side, 0.0f)));
		if (std::abs(left - _panLeft) > 2 || std::abs(right - _panRight) > 2)
		{
			Mix_SetPanning(FirstChannel + FIRE_NEAR, (Uint8)left, (Uint8)right);
			Mix_SetPanning(FirstChannel + FIRE_FAR, (Uint8)left, (Uint8)right);
			_panLeft = left;
			_panRight = right;
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
