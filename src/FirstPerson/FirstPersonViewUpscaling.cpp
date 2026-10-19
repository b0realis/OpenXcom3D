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
 * FirstPersonViewUpscaling.cpp: smoother sprites up close (an option).
 *
 * Up close, unit and item frames are drawn from a version four times bigger (xBRZ), made on a
 * worker thread and cached, so they look smooth instead of blocky.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <SDL.h>
#include "../Engine/Options.h"
#include "../Engine/Scalers/xbrz.h"
#include "../Engine/Surface.h"
#include "../Savegame/BattleUnit.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * Is a sprite at this point big enough on screen to be worth upscaling? Only with the option
 * "Sprite upscaling" on, and only when one sprite pixel covers at least UpscaleFrom (1.6) screen
 * pixels across: further away the original's pixels are too small to look blocky.
 * @param world The sprite's anchor point, in voxels.
 * @param scale How much bigger than usual the sprite is drawn.
 */
bool FirstPersonView::upscaleHere(const double world[3], double scale) const
{
	if (Options::fpsSpriteUpscale <= 0)
	{
		return false;
	}
	// Where the point is on the screen, and how far in front of the camera (behind it: no).
	double screenX, screenY, depth;
	if (!projectToScreen(world, screenX, screenY, depth))
	{
		return false;
	}
	// Screen pixels per sprite pixel across, as drawSprite works it out (an isometric pixel is
	// 1/sqrt(2) of a voxel across).
	return (getWidth() / 2.0) / _halfFovTangent / depth * scale * SpritePixelX >= UpscaleFrom;
}

/**
 * Draws a sprite standing in the world, from its upscaled version when it's close (see
 * upscaleHere) and that version is ready; otherwise the original, as drawSprite would.
 * @param sprite The sprite frame (null draws nothing).
 * @param anchorX, anchorY The sprite pixel that sits at the world point.
 * @param world The world point, in voxels.
 * @param depthBias How much nearer than the point the sprite counts for the depth test, in voxels.
 * @param shade How much darker to draw it (the game's shade, 0 none).
 */
void FirstPersonView::drawSpriteSmooth(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade)
{
	if (sprite && upscaleHere(world, 1.0))
	{
		// The bigger version, drawn at 1/UpscaleFactor scale so it covers the same screen area,
		// its anchor scaled up to match.
		if (const Surface *bigSprite = upscaled(sprite))
		{
			drawSprite(bigSprite, anchorX * UpscaleFactor, anchorY * UpscaleFactor, world, depthBias, shade, 1.0 / UpscaleFactor);
			return;
		}
	}
	drawSprite(sprite, anchorX, anchorY, world, depthBias, shade);
}

/**
 * Gets a sprite frame made bigger the way pixel art scales well (setting "Sprite upscaling"):
 * xBRZ rounds the stair-stepped edges along the shapes the artist drew, without blurring; the
 * result goes back into the game's palette (each colour to its nearest), and with "Smooth +
 * detail" the shading is sharpened a little first. The frame is made UpscaleFactor (4) times
 * bigger, unshaded, so the light is applied when it's drawn.
 *
 * The work is done on a worker thread (upscaleWorker), so a frame never waits for it. Until a
 * frame is ready this returns null and the caller draws the original. Called on the main
 * thread only. How the two threads share the work:
 * - _upscaledSprites (the finished surfaces) and _upscaleClock belong to the main thread
 *   alone, so they need no lock.
 * - Everything the worker sees is guarded by _upscaleMutex: _upscaleJobs (frames waiting to be
 *   made bigger, oldest first), _upscaleQueued (the keys of frames waiting or being worked on,
 *   so a frame is queued only once), _upscaleDone (finished pixels waiting to be picked up
 *   here) and _upscaleStop (set by the destructor).
 * - A job carries its own copy of the frame's pixels and the palette, so the worker never
 *   touches a Surface or the view's palette.
 * - _upscaleWake wakes the worker when a job is queued, and when the destructor tells it to
 *   stop (then joins the thread).
 * Frames are found again by their pixels (a hash of them), not by their surface: units and
 * items are first drawn into a shared buffer (_spriteBuffer), so one surface holds a different
 * frame each time, and two units showing the same frame share its upscaled version.
 * @param sprite The sprite frame (8-bit, in the view's palette).
 * @return The upscaled frame, owned by the cache; null while it's not ready yet.
 */
const Surface *FirstPersonView::upscaled(const Surface *sprite)
{
	const SDL_Surface *source = const_cast<Surface*>(sprite)->getSurface(); // read only
	const int width = source->w, height = source->h, factor = UpscaleFactor;
	const bool detail = Options::fpsSpriteUpscale >= 2;

	// A new palette: everything made for the old one goes, finished, waiting or queued (a job
	// the worker is busy with is dropped when it's done, as its key is no longer queued: the
	// palette is part of the key, so the same frame queued again meanwhile has another one).
	if (_upscalePalette != _colors.paletteHash)
	{
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		for (auto &entry : _upscaledSprites) delete entry.second.surface;
		_upscaledSprites.clear();
		_upscaleDone.clear();
		_upscaleQueued.clear();
		_upscaleJobs.clear();
		_upscalePalette = _colors.paletteHash;
	}

	// The cache key: a 64-bit FNV-1a hash of the frame's pixels, seeded with its size, the
	// detail setting and the palette (so a frame the worker made for an old palette never
	// passes for the same frame in the new one).
	Uint64 key = 1469598103934665603ULL ^ ((Uint64)width << 32) ^ ((Uint64)height << 48) ^ (detail ? 1 : 0);
	key = (key ^ _colors.paletteHash) * 1099511628211ULL;
	for (int y = 0; y < height; ++y)
	{
		const Uint8 *row = (const Uint8 *)source->pixels + y * source->pitch;
		for (int x = 0; x < width; ++x) key = (key ^ row[x]) * 1099511628211ULL;
	}

	// Made before: mark it as just used and hand it out.
	auto cached = _upscaledSprites.find(key);
	if (cached != _upscaledSprites.end())
	{
		cached->second.used = ++_upscaleClock;
		return cached->second.surface;
	}

	// Ask the worker, under the lock: is it done? Then take its pixels. If not, queue the frame
	// unless it's queued already or the queue is full (64 jobs), and draw the original for now.
	std::vector<Uint8> upscaledPixels;
	{
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		auto ready = _upscaleDone.find(key);
		if (ready != _upscaleDone.end())
		{
			upscaledPixels.swap(ready->second);
			_upscaleDone.erase(ready);
			_upscaleQueued.erase(key);
		}
		else
		{
			if (!_upscaleQueued.count(key) && _upscaleJobs.size() < 64)
			{
				// The job: the frame's pixels row by row without the surface's padding, and the
				// current palette.
				UpscaleJob job;
				job.key = key; job.w = width; job.h = height; job.detail = detail; job.paletteHash = _colors.paletteHash;
				job.pixels.resize(width * height);
				for (int y = 0; y < height; ++y)
				{
					std::copy((const Uint8 *)source->pixels + y * source->pitch, (const Uint8 *)source->pixels + y * source->pitch + width,
						job.pixels.begin() + y * width);
				}
				const SDL_Color *palette = getPalette();
				job.palette.assign(palette, palette + 256);
				_upscaleJobs.push_back(std::move(job));
				_upscaleQueued.insert(key);
				// The worker thread starts with the first job; wake it (it may be asleep).
				if (!_upscaleThread.joinable()) _upscaleThread = std::thread(&FirstPersonView::upscaleWorker, this);
				_upscaleWake.notify_one();
			}
			return nullptr;
		}
	}

	// It's done: copy the pixels into a new surface, row by row.
	Surface *result = new Surface(width * factor, height * factor);
	SDL_Surface *target = result->getSurface();
	for (int y = 0; y < height * factor; ++y)
	{
		std::copy(upscaledPixels.begin() + y * width * factor, upscaledPixels.begin() + (y + 1) * width * factor,
			(Uint8 *)target->pixels + y * target->pitch);
	}

	// A few hundred frames (400) are kept; when full, the one unused longest goes.
	if (_upscaledSprites.size() >= 400)
	{
		auto oldest = _upscaledSprites.begin();
		for (auto entry = _upscaledSprites.begin(); entry != _upscaledSprites.end(); ++entry)
		{
			if (entry->second.used < oldest->second.used) oldest = entry;
		}
		delete oldest->second.surface;
		_upscaledSprites.erase(oldest);
	}
	_upscaledSprites.emplace(key, Upscaled{ result, ++_upscaleClock });
	return result;
}

/**
 * The upscaling worker thread's loop: sleeps until a job is queued, takes it off the queue,
 * makes the frame's bigger version (see upscaled) without holding the lock, and hands the
 * pixels back through _upscaleDone. Runs at low priority (on Linux) until the destructor sets
 * _upscaleStop. It keeps its own nearest-colour table for the palette the jobs come with.
 */
void FirstPersonView::upscaleWorker()
{
#ifdef __linux__
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15); // spare time only: the frame's threads come first
#endif
	// The nearest palette colour to each 15-bit colour (5 bits each of red, green, blue), and
	// the palette it was made for.
	std::vector<Uint8> nearestColor;
	Uint64 nearestColorPalette = 0;
	while (true)
	{
		// Sleep until there is a job or it's time to stop (the wait lets go of the lock while
		// asleep and checks again on waking, so a spurious wake-up does no harm); then take the
		// oldest job and let go of the lock.
		UpscaleJob job;
		{
			std::unique_lock<std::mutex> lock(_upscaleMutex);
			_upscaleWake.wait(lock, [this] { return _upscaleStop || !_upscaleJobs.empty(); });
			if (_upscaleStop) return;
			job = std::move(_upscaleJobs.front());
			_upscaleJobs.pop_front();
		}

		// A job with another palette: make the nearest-colour table again. Colour 0 is
		// transparent, so it is never picked; the distance weighs green most and blue least,
		// roughly as the eye does.
		const std::vector<SDL_Color> &palette = job.palette;
		if (nearestColor.empty() || nearestColorPalette != job.paletteHash)
		{
			nearestColor.assign(32768, 0);
			for (int rgb15 = 0; rgb15 < 32768; ++rgb15)
			{
				const int red = ((rgb15 >> 10) & 31) * 255 / 31, green = ((rgb15 >> 5) & 31) * 255 / 31, blue = (rgb15 & 31) * 255 / 31;
				int bestIndex = 1, bestDistance = INT_MAX;
				for (int index = 1; index < 256; ++index)
				{
					const int dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
					const int distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
					if (distance < bestDistance) { bestDistance = distance; bestIndex = index; }
				}
				nearestColor[rgb15] = (Uint8)bestIndex;
			}
			nearestColorPalette = job.paletteHash;
		}

		// The frame in true colour (ARGB, transparent pixels fully transparent), made bigger by
		// xBRZ.
		const int width = job.w, height = job.h, factor = UpscaleFactor, bigWidth = width * factor, bigHeight = height * factor;
		std::vector<uint32_t> sourceArgb(width * height), bigArgb(bigWidth * bigHeight);
		for (int pixel = 0; pixel < width * height; ++pixel)
		{
			const Uint8 color = job.pixels[pixel];
			sourceArgb[pixel] = color
				? 0xFF000000u | ((uint32_t)palette[color].r << 16) | ((uint32_t)palette[color].g << 8) | palette[color].b
				: 0u;
		}
		xbrz::scale(factor, sourceArgb.data(), bigArgb.data(), width, height, xbrz::ARGB);

		// The big frame's colours as floats (red, green, blue per pixel); fully transparent
		// pixels 0. xBRZ blends an edge with the transparency around it in its alpha only: the
		// colour is the opaque neighbours' own (weighted by their alpha), not darkened, so it's
		// taken as it is.
		std::vector<float> bigRgb(bigWidth * bigHeight * 3);
		for (int pixel = 0; pixel < bigWidth * bigHeight; ++pixel)
		{
			const uint32_t argb = bigArgb[pixel];
			for (int channel = 0; channel < 3; ++channel)
			{
				bigRgb[pixel * 3 + channel] = (argb >> 24) ? (float)((argb >> (16 - 8 * channel)) & 255) : 0.0f;
			}
		}

		// A big pixel is part of the sprite (1) if it is at least half opaque, else
		// transparent (0).
		auto isSolid = [&](int pixel) { return (bigArgb[pixel] >> 24) >= 128 ? 1.0f : 0.0f; };

		if (job.detail)
		{
			// Unsharp mask: each colour pushed away from the 5x5 average of the sprite around it
			// (only the sprite's own pixels count, so outlines don't get a rim of light). The
			// average is a box blur done in two passes, along the rows and then down the
			// columns; each entry holds red, green and blue sums and the number of solid pixels.
			std::vector<float> rowSums(bigWidth * bigHeight * 4), boxSums(bigWidth * bigHeight * 4);
			const int radius = 2;

			// Pass 1: sum each pixel's 5 neighbours along its row (clamped at the edges).
			for (int y = 0; y < bigHeight; ++y)
				for (int x = 0; x < bigWidth; ++x)
				{
					float sum[4] = {};
					for (int offset = -radius; offset <= radius; ++offset)
					{
						const int neighbour = y * bigWidth + std::clamp(x + offset, 0, bigWidth - 1);
						const float weight = isSolid(neighbour);
						for (int channel = 0; channel < 3; ++channel) sum[channel] += bigRgb[neighbour * 3 + channel] * weight;
						sum[3] += weight;
					}
					for (int channel = 0; channel < 4; ++channel) rowSums[(y * bigWidth + x) * 4 + channel] = sum[channel];
				}

			// Pass 2: sum those down the column, giving the 5x5 sums.
			for (int y = 0; y < bigHeight; ++y)
				for (int x = 0; x < bigWidth; ++x)
				{
					float sum[4] = {};
					for (int offset = -radius; offset <= radius; ++offset)
					{
						const int neighbour = std::clamp(y + offset, 0, bigHeight - 1) * bigWidth + x;
						for (int channel = 0; channel < 4; ++channel) sum[channel] += rowSums[neighbour * 4 + channel];
					}
					for (int channel = 0; channel < 4; ++channel) boxSums[(y * bigWidth + x) * 4 + channel] = sum[channel];
				}

			// Push each solid pixel's colour 70% further away from the average, where it differs
			// by more than 3 (smaller differences are left alone, so flat areas stay flat).
			for (int pixel = 0; pixel < bigWidth * bigHeight; ++pixel)
			{
				if (!isSolid(pixel) || boxSums[pixel * 4 + 3] <= 0.0f) continue;
				for (int channel = 0; channel < 3; ++channel)
				{
					const float difference = bigRgb[pixel * 3 + channel] - boxSums[pixel * 4 + channel] / boxSums[pixel * 4 + 3];
					if (std::abs(difference) > 3.0f)
						bigRgb[pixel * 3 + channel] = std::clamp(bigRgb[pixel * 3 + channel] + 0.7f * difference, 0.0f, 255.0f);
				}
			}
		}

		// Back into the palette: each solid pixel to the nearest colour of its 15-bit colour,
		// transparent pixels 0.
		std::vector<Uint8> result(bigWidth * bigHeight, 0);
		for (int pixel = 0; pixel < bigWidth * bigHeight; ++pixel)
		{
			if (!isSolid(pixel)) continue;
			const int red = (int)std::clamp(bigRgb[pixel * 3], 0.0f, 255.0f), green = (int)std::clamp(bigRgb[pixel * 3 + 1], 0.0f, 255.0f),
				blue = (int)std::clamp(bigRgb[pixel * 3 + 2], 0.0f, 255.0f);
			result[pixel] = nearestColor[((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3)];
		}

		// Hand it back under the lock, if it's still wanted: a palette change meanwhile empties
		// _upscaleQueued (see upscaled).
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		if (_upscaleQueued.count(job.key)) _upscaleDone[job.key] = std::move(result); // (dropped if the palette changed meanwhile)
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
