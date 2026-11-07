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
 * Up close, unit and item frames are drawn from a bigger version (xBRZ: four times, six with
 * "Finest"), made on a worker thread and cached, so they look smooth instead of blocky. The bigger
 * version stays in the game's palette the way the art uses it: every pixel takes a shade of one of
 * the 16-shade ramps the original has right there, so no stray colour of another ramp appears at
 * an edge, and recolours and the view's shading (both work on ramps) treat it as the original.
 * Inside a ramp the shading steps are softened (never across an outline or another colour), and
 * with "Smooth + detail" and "Finest" sharpened a little; "Finest" also keeps 1-pixel features
 * (eyes, highlights) at their own shade.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // (std::min and std::max, not Windows' macros)
#endif
#include <windows.h>
#endif
#include <SDL.h>
#include "../Engine/Options.h"
#include "../Engine/Scalers/xbrz.h"
#include "../Engine/Surface.h"
#include "../Savegame/BattleUnit.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/// The colours of 15-bit colour space (5 bits each of red, green, blue) and of the 16 ramps.
const int Colors15Bit = 32768, RampCount = 16, ShadesPerRamp = 16;

/**
 * The colour distance the upscaler picks palette colours by: green weighs most and blue least,
 * roughly as the eye does.
 */
inline int colorDistance(int red1, int green1, int blue1, int red2, int green2, int blue2)
{
	const int dr = red1 - red2, dg = green1 - green2, db = blue1 - blue2;
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

/**
 * A colour's 15-bit index (5 bits each of red, green, blue), its channels cut to 0..255 first.
 */
inline int colorIndex15Bit(float red, float green, float blue)
{
	const int r = (int)std::clamp(red, 0.0f, 255.0f), g = (int)std::clamp(green, 0.0f, 255.0f), b = (int)std::clamp(blue, 0.0f, 255.0f);
	return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

/**
 * The colour a 15-bit index stands for (each channel's 5 bits spread over 0..255).
 */
inline void color15Bit(int index15, int &red, int &green, int &blue)
{
	red = ((index15 >> 10) & 31) * 255 / 31;
	green = ((index15 >> 5) & 31) * 255 / 31;
	blue = (index15 & 31) * 255 / 31;
}

/**
 * The upscaler's tables for one palette: the nearest palette colour to each 15-bit colour, and
 * the nearest shade of each ramp to it (colour 0, transparent, is never picked).
 */
struct UpscalePaletteTables
{
	std::vector<Uint8> nearest;     // 15-bit colour -> palette index
	std::vector<Uint8> rampNearest; // ramp * Colors15Bit + 15-bit colour -> palette index (a shade of that ramp)

	/**
	 * Makes both tables for a palette.
	 * @param palette The palette (256 colours).
	 */
	void make(const std::vector<SDL_Color> &palette)
	{
		nearest.assign(Colors15Bit, 0);
		rampNearest.assign(RampCount * Colors15Bit, 0);
		for (int index15 = 0; index15 < Colors15Bit; ++index15)
		{
			int red, green, blue;
			color15Bit(index15, red, green, blue);
			// The whole palette (but colour 0).
			int bestIndex = 1, bestDistance = INT_MAX;
			for (int index = 1; index < 256; ++index)
			{
				const int distance = colorDistance(palette[index].r, palette[index].g, palette[index].b, red, green, blue);
				if (distance < bestDistance) { bestDistance = distance; bestIndex = index; }
			}
			nearest[index15] = (Uint8)bestIndex;
			// Each ramp's 16 shades (ramp 0 without colour 0).
			for (int ramp = 0; ramp < RampCount; ++ramp)
			{
				int bestShade = ramp * ShadesPerRamp + (ramp == 0 ? 1 : 0), bestShadeDistance = INT_MAX;
				for (int shade = 0; shade < ShadesPerRamp; ++shade)
				{
					const int index = ramp * ShadesPerRamp + shade;
					if (index == 0) continue;
					const int distance = colorDistance(palette[index].r, palette[index].g, palette[index].b, red, green, blue);
					if (distance < bestShadeDistance) { bestShadeDistance = distance; bestShade = index; }
				}
				rampNearest[ramp * Colors15Bit + index15] = (Uint8)bestShade;
			}
		}
	}

	/**
	 * The nearest colour to a colour among the shades of some ramps (by its 15-bit colour, as
	 * the tables): each ramp's nearest shade, the nearest of those.
	 * @param ramps The ramps to choose from, a bit for each (bit r: ramp r).
	 * @param red, green, blue The colour.
	 * @param palette The palette the tables were made for.
	 * @return The palette index (0 if no ramp was given).
	 */
	Uint8 nearestAmongRamps(Uint16 ramps, float red, float green, float blue, const std::vector<SDL_Color> &palette) const
	{
		const int index15 = colorIndex15Bit(red, green, blue);
		int r, g, b;
		color15Bit(index15, r, g, b);
		int best = 0, bestDistance = INT_MAX;
		for (int ramp = 0; ramp < RampCount; ++ramp)
		{
			if (!(ramps & (1 << ramp))) continue;
			const Uint8 index = rampNearest[ramp * Colors15Bit + index15];
			const int distance = colorDistance(palette[index].r, palette[index].g, palette[index].b, r, g, b);
			if (distance < bestDistance) { bestDistance = distance; best = index; }
		}
		return (Uint8)best;
	}
};

/**
 * A palette colour's brightness (luminance), for finding small features.
 */
inline float colorLuminance(const SDL_Color &color)
{
	return 0.299f * color.r + 0.587f * color.g + 0.114f * color.b;
}

/**
 * The buffers one frame is made bigger in (kept by the worker from frame to frame).
 */
struct UpscaleBuffers
{
	std::vector<uint32_t> sourceArgb, bigArgb; // the frame in true colour, and xBRZ's bigger one
	std::vector<float> bigRgb, softened;       // the bigger frame's colours (red, green, blue per pixel), and a scratch copy
	std::vector<float> boxSums;                // the detail pass's sums around each pixel (red, green, blue, count)
	std::vector<Uint8> solid, rampClass, keep; // per big pixel: part of the sprite; its shade class (a palette index); a small feature's
	std::vector<Uint8> smallFeature;           // per original pixel: a 1-pixel feature
};

/**
 * Makes a sprite frame bigger the way the setting "Sprite upscaling" wants it, into palette
 * indices (0 transparent):
 * 1. xBRZ makes it `factor` times bigger, in true colour; a big pixel is part of the sprite if
 *    it is at least half opaque.
 * 2. Each big pixel's ramps: those of the 3x3 original pixels around the one under it. Its class
 *    is the nearest shade of xBRZ's colour among them (so an edge between two colours takes one
 *    of the two, never a third colour that only looks alike).
 * 3. With keepFeatures, the 1-pixel features (an original pixel brighter or darker than all its
 *    neighbours by UpscaleFeatureContrast) keep xBRZ's colours through the next two steps.
 * 4. The shading steps inside a ramp are softened: each colour moves to a Gaussian average of its
 *    neighbours of the same ramp at most one shade away (along the rows, then down the columns),
 *    so an original pixel's shade no longer shows as a block; outlines and other colours, more
 *    than a shade away or of another ramp, never mix in.
 * 5. With detail, an unsharp mask: each colour pushed away from the average of its neighbours of
 *    the same ramp, where they differ by more than a little.
 * 6. Back into the palette: each big pixel the nearest shade of its class's ramp.
 * Local (no step reaches further than about two original pixels) and deterministic: the same frame
 * always comes out the same, so an animation shows no flicker.
 * @param pixels The frame's pixels (palette indices, row by row).
 * @param width, height The frame's size.
 * @param palette The palette (256 colours).
 * @param tables The palette's tables (see UpscalePaletteTables).
 * @param factor How much bigger (UpscaleFactor or FinestUpscaleFactor).
 * @param detail With the unsharp mask (step 5).
 * @param keepFeatures With the small features kept (step 3).
 * @param buffers Scratch buffers (reused from frame to frame).
 * @param result The bigger frame's pixels (width * factor x height * factor, row by row).
 */
void upscaleFrame(const std::vector<Uint8> &pixels, int width, int height, const std::vector<SDL_Color> &palette, const UpscalePaletteTables &tables,
	int factor, bool detail, bool keepFeatures, UpscaleBuffers &buffers, std::vector<Uint8> &result)
{
	const int bigWidth = width * factor, bigHeight = height * factor, bigCount = bigWidth * bigHeight;
	std::vector<uint32_t> &sourceArgb = buffers.sourceArgb, &bigArgb = buffers.bigArgb;
	std::vector<float> &bigRgb = buffers.bigRgb, &softened = buffers.softened;
	std::vector<Uint8> &solid = buffers.solid, &rampClass = buffers.rampClass, &keep = buffers.keep;
	sourceArgb.resize(width * height);
	bigArgb.resize(bigCount);
	bigRgb.resize(bigCount * 3);
	solid.resize(bigCount);
	rampClass.assign(bigCount, 0);
	keep.assign(bigCount, 0);

	// 1. The frame in true colour (ARGB, transparent pixels fully transparent), made bigger by xBRZ.
	// xBRZ blends an edge with the transparency around it in its alpha only (the colour is the
	// opaque neighbours' own), so a big pixel's colour is taken as it is; transparent ones are 0.
	for (int pixel = 0; pixel < width * height; ++pixel)
	{
		const Uint8 color = pixels[pixel];
		sourceArgb[pixel] = color
			? 0xFF000000u | ((uint32_t)palette[color].r << 16) | ((uint32_t)palette[color].g << 8) | palette[color].b
			: 0u;
	}
	xbrz::scale(factor, sourceArgb.data(), bigArgb.data(), width, height, xbrz::ARGB);
	for (int pixel = 0; pixel < bigCount; ++pixel)
	{
		const uint32_t argb = bigArgb[pixel];
		solid[pixel] = (argb >> 24) >= 128;
		for (int channel = 0; channel < 3; ++channel)
		{
			bigRgb[pixel * 3 + channel] = (argb >> 24) ? (float)((argb >> (16 - 8 * channel)) & 255) : 0.0f;
		}
	}

	// 2. Each solid big pixel's class: the nearest shade of its colour among the ramps of the 3x3
	// original pixels around the one under it.
	for (int bigY = 0; bigY < bigHeight; ++bigY)
		for (int bigX = 0; bigX < bigWidth; ++bigX)
		{
			const int pixel = bigY * bigWidth + bigX;
			if (!solid[pixel]) continue;
			const int sourceX = bigX / factor, sourceY = bigY / factor;
			Uint16 ramps = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int x = sourceX + dx, y = sourceY + dy;
					if (x < 0 || y < 0 || x >= width || y >= height) continue;
					const Uint8 color = pixels[y * width + x];
					if (color) ramps |= 1 << (color >> 4);
				}
			rampClass[pixel] = ramps ? tables.nearestAmongRamps(ramps, bigRgb[pixel * 3], bigRgb[pixel * 3 + 1], bigRgb[pixel * 3 + 2], palette) : 0;
		}

	// 3. The small features: an original pixel brighter or darker than every one of its solid
	// neighbours (at least UpscaleFeatureNeighbours of the 8) by UpscaleFeatureContrast in
	// luminance; its big pixels of its own ramp are kept as xBRZ made them.
	if (keepFeatures)
	{
		std::vector<Uint8> &smallFeature = buffers.smallFeature;
		smallFeature.assign(width * height, 0);
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const Uint8 color = pixels[y * width + x];
				if (!color) continue;
				const float luminance = colorLuminance(palette[color]);
				bool brighter = true, darker = true;
				int neighbours = 0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = x + dx, ny = y + dy;
						if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
						const Uint8 other = pixels[ny * width + nx];
						if (!other) continue;
						++neighbours;
						const float otherLuminance = colorLuminance(palette[other]);
						brighter = brighter && luminance >= otherLuminance + UpscaleFeatureContrast;
						darker = darker && luminance <= otherLuminance - UpscaleFeatureContrast;
					}
				smallFeature[y * width + x] = neighbours >= UpscaleFeatureNeighbours && (brighter || darker);
			}
		for (int bigY = 0; bigY < bigHeight; ++bigY)
			for (int bigX = 0; bigX < bigWidth; ++bigX)
			{
				const int pixel = bigY * bigWidth + bigX;
				const int source = (bigY / factor) * width + bigX / factor;
				keep[pixel] = solid[pixel] && smallFeature[source] && rampClass[pixel] && (rampClass[pixel] >> 4) == (pixels[source] >> 4);
			}
	}

	// 4. The shading softened inside each ramp: a Gaussian (sigma half a factor, as far as a
	// factor) along the rows and then down the columns, over the neighbours that are solid, not a
	// kept feature, of the pixel's ramp and at most UpscaleSoftenShades shades from its class.
	{
		const int radius = factor;
		const float sigma = std::max(0.5f, radius * 0.5f);
		std::vector<float> weights(2 * radius + 1);
		for (int offset = -radius; offset <= radius; ++offset)
		{
			weights[offset + radius] = std::exp(-(offset * offset) / (2 * sigma * sigma));
		}
		std::vector<float> passIn, passOut;
		for (int pass = 0; pass < 2; ++pass)
		{
			// (pass 0 reads the colours, pass 1 the rows' result)
			const std::vector<float> &from = pass == 0 ? bigRgb : passIn;
			passOut = from;
			for (int bigY = 0; bigY < bigHeight; ++bigY)
				for (int bigX = 0; bigX < bigWidth; ++bigX)
				{
					const int pixel = bigY * bigWidth + bigX;
					if (!solid[pixel] || !rampClass[pixel] || keep[pixel]) continue;
					const int ramp = rampClass[pixel] >> 4, shade = rampClass[pixel] & 15;
					float sum[3] = { 0, 0, 0 }, weightSum = 0;
					for (int offset = -radius; offset <= radius; ++offset)
					{
						const int x = pass == 0 ? bigX + offset : bigX, y = pass == 0 ? bigY : bigY + offset;
						if (x < 0 || y < 0 || x >= bigWidth || y >= bigHeight) continue;
						const int neighbour = y * bigWidth + x;
						if (!solid[neighbour] || keep[neighbour] || (rampClass[neighbour] >> 4) != ramp
							|| std::abs((rampClass[neighbour] & 15) - shade) > UpscaleSoftenShades) continue;
						for (int channel = 0; channel < 3; ++channel) sum[channel] += from[neighbour * 3 + channel] * weights[offset + radius];
						weightSum += weights[offset + radius];
					}
					if (weightSum > 0)
					{
						for (int channel = 0; channel < 3; ++channel) passOut[pixel * 3 + channel] = sum[channel] / weightSum;
					}
				}
			passIn.swap(passOut);
		}
		// Each colour moves UpscaleSoftenAmount of the way to its softened one.
		softened.resize(bigCount * 3);
		for (int value = 0; value < bigCount * 3; ++value)
		{
			softened[value] = bigRgb[value] + UpscaleSoftenAmount * (passIn[value] - bigRgb[value]);
		}
		bigRgb.swap(softened);
	}

	// 5. The detail pass: each solid pixel's colour pushed UpscaleDetailAmount further away from the
	// average of its neighbours of the same ramp (a box as far as half a factor, clamped at the
	// frame's edges), where they differ by more than UpscaleDetailThreshold (small differences are
	// left alone, so flat areas stay flat); kept features are left as they are.
	if (detail)
	{
		const int radius = factor / 2;
		std::vector<float> &boxSums = buffers.boxSums;
		boxSums.assign(bigCount * 4, 0.0f);
		for (int bigY = 0; bigY < bigHeight; ++bigY)
			for (int bigX = 0; bigX < bigWidth; ++bigX)
			{
				const int pixel = bigY * bigWidth + bigX;
				if (!solid[pixel]) continue;
				const int ramp = rampClass[pixel] >> 4;
				float sum[4] = {};
				for (int dy = -radius; dy <= radius; ++dy)
					for (int dx = -radius; dx <= radius; ++dx)
					{
						const int x = std::clamp(bigX + dx, 0, bigWidth - 1), y = std::clamp(bigY + dy, 0, bigHeight - 1);
						const int neighbour = y * bigWidth + x;
						if (!solid[neighbour] || (rampClass[neighbour] >> 4) != ramp) continue;
						for (int channel = 0; channel < 3; ++channel) sum[channel] += bigRgb[neighbour * 3 + channel];
						sum[3] += 1;
					}
				for (int channel = 0; channel < 4; ++channel) boxSums[pixel * 4 + channel] = sum[channel];
			}
		for (int pixel = 0; pixel < bigCount; ++pixel)
		{
			if (!solid[pixel] || keep[pixel] || boxSums[pixel * 4 + 3] <= 0.0f) continue;
			for (int channel = 0; channel < 3; ++channel)
			{
				const float difference = bigRgb[pixel * 3 + channel] - boxSums[pixel * 4 + channel] / boxSums[pixel * 4 + 3];
				if (std::abs(difference) > UpscaleDetailThreshold)
				{
					bigRgb[pixel * 3 + channel] = std::clamp(bigRgb[pixel * 3 + channel] + UpscaleDetailAmount * difference, 0.0f, 255.0f);
				}
			}
		}
	}

	// 6. Back into the palette: each solid pixel the nearest shade of its class's ramp (a pixel
	// without a class, none in practice, the nearest colour of all), transparent pixels 0.
	result.assign(bigCount, 0);
	for (int pixel = 0; pixel < bigCount; ++pixel)
	{
		if (!solid[pixel]) continue;
		const int index15 = colorIndex15Bit(bigRgb[pixel * 3], bigRgb[pixel * 3 + 1], bigRgb[pixel * 3 + 2]);
		result[pixel] = rampClass[pixel] ? tables.rampNearest[(rampClass[pixel] >> 4) * Colors15Bit + index15] : tables.nearest[index15];
	}
}

}

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
	return focalPixels() / depth * scale * SpritePixelX >= UpscaleFrom;
}

/**
 * How much bigger units', items' and effects' frames are made for the setting "Sprite upscaling":
 * FinestUpscaleFactor (6) with "Finest", else UpscaleFactor (4). (The soft smoke frames always use
 * UpscaleFactor.)
 */
int FirstPersonView::spriteUpscaleFactor() const
{
	return Options::fpsSpriteUpscale >= 3 ? FinestUpscaleFactor : UpscaleFactor;
}

/**
 * Draws a sprite standing in the world, from its upscaled version when it's close (see
 * upscaleHere) and that version is ready; otherwise the original, as drawSprite would.
 * @param sprite The sprite frame (null draws nothing).
 * @param anchorX, anchorY The sprite pixel that sits at the world point.
 * @param world The world point, in voxels.
 * @param depthBias How much nearer than the point the sprite counts for the depth test, in voxels.
 * @param shade How much darker to draw it (the game's shade, 0 none).
 * @param scale Its size against the isometric map's (1: the same; see drawSprite).
 * @param pose Standing, or lying on the floor (see drawSprite).
 */
void FirstPersonView::drawSpriteSmooth(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade, double scale,
	SpritePose pose)
{
	SpriteDraw draw;
	draw.sprite = sprite;
	draw.anchorX = anchorX;
	draw.anchorY = anchorY;
	std::copy(world, world + 3, draw.world);
	draw.depthBias = depthBias;
	draw.shade = shade;
	draw.scale = scale;
	draw.pose = pose;
	useSmoothVersion(draw);
	drawSprite(draw);
}

/**
 * Swaps a sprite about to be drawn for its upscaled version, when it's close (see upscaleHere)
 * and that version is ready: drawn at 1/factor scale so it covers the same screen area, its
 * anchor scaled up to match (the factor: see spriteUpscaleFactor). Otherwise it stays as it is.
 * @param draw The sprite to draw (see SpriteDraw); its sprite, anchor and scale may change.
 */
void FirstPersonView::useSmoothVersion(SpriteDraw &draw)
{
	if (draw.sprite && upscaleHere(draw.world, 1.0))
	{
		const int factor = spriteUpscaleFactor();
		if (const Surface *bigSprite = upscaled(draw.sprite, factor))
		{
			draw.sprite = bigSprite;
			draw.anchorX *= factor;
			draw.anchorY *= factor;
			draw.scale /= factor;
		}
	}
}

/**
 * Gets a sprite frame made bigger the way pixel art scales well (setting "Sprite upscaling":
 * see upscaleFrame for the steps): "Smooth" makes it UpscaleFactor (4) times bigger with its
 * shading softened; "Smooth + detail" sharpens it a little too; "Finest" does that
 * FinestUpscaleFactor (6) times bigger (for units, items and effects; the soft smoke frames ask
 * for UpscaleFactor) and keeps 1-pixel features at their shade. The frame is made unshaded, so
 * the light is applied when it's drawn.
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
 * @param factor How much bigger: spriteUpscaleFactor() for units, items and effects, UpscaleFactor
 * for the soft smoke frames.
 * @return The upscaled frame, owned by the cache; null while it's not ready yet.
 */
const Surface *FirstPersonView::upscaled(const Surface *sprite, int factor)
{
	const SDL_Surface *source = const_cast<Surface*>(sprite)->getSurface(); // read only
	const int width = source->w, height = source->h;
	const bool detail = Options::fpsSpriteUpscale >= 2, keepFeatures = Options::fpsSpriteUpscale >= 3;

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

	// The cache key: a 64-bit FNV-1a hash of the frame's pixels, seeded with its size, the factor,
	// the steps the setting asks for and the palette (so a frame the worker made for an old
	// palette or another setting never passes for the same frame now).
	Uint64 key = 1469598103934665603ULL ^ ((Uint64)width << 32) ^ ((Uint64)height << 48) ^ ((Uint64)factor << 8) ^ (detail ? 1 : 0)
		^ (keepFeatures ? 2 : 0);
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
				// The job: the frame's pixels row by row without the surface's padding, the
				// current palette, and how it's to be made.
				UpscaleJob job;
				job.key = key; job.w = width; job.h = height; job.factor = factor; job.detail = detail; job.keepFeatures = keepFeatures;
				job.paletteHash = _colors.paletteHash;
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

	// UpscaleCacheFrames (400) frames are kept (a unit's 64x64 frame is 64 KB at 4x, 147 KB at 6x:
	// about 26 or 59 MB when full); when full, the one unused longest goes (never one used this
	// frame: a unit recorded to be drawn may still point at it, see drawUnits).
	if (_upscaledSprites.size() >= UpscaleCacheFrames)
	{
		auto oldest = _upscaledSprites.begin();
		for (auto entry = _upscaledSprites.begin(); entry != _upscaledSprites.end(); ++entry)
		{
			if (entry->second.used < oldest->second.used) oldest = entry;
		}
		if (oldest->second.used < _upscaleFrameStart)
		{
			delete oldest->second.surface;
			_upscaledSprites.erase(oldest);
		}
	}
	_upscaledSprites.emplace(key, Upscaled{ result, ++_upscaleClock });
	return result;
}

/**
 * The upscaling worker thread's loop: sleeps until a job is queued, takes it off the queue,
 * makes the frame's bigger version (see upscaleFrame) without holding the lock, and hands the
 * pixels back through _upscaleDone. Runs at low priority (Linux, Windows) until the destructor sets
 * _upscaleStop. It keeps its own tables for the palette the jobs come with, and its scratch
 * buffers from job to job.
 */
void FirstPersonView::upscaleWorker()
{
#ifdef __linux__
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15); // spare time only: the frame's threads come first
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
	// The nearest-colour tables, and the palette they were made for.
	UpscalePaletteTables tables;
	Uint64 tablesPalette = 0;
	UpscaleBuffers buffers;
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

		// A job with another palette: make the tables again.
		if (tables.nearest.empty() || tablesPalette != job.paletteHash)
		{
			tables.make(job.palette);
			tablesPalette = job.paletteHash;
		}

		// The bigger frame.
		std::vector<Uint8> result;
		upscaleFrame(job.pixels, job.w, job.h, job.palette, tables, job.factor, job.detail, job.keepFeatures, buffers, result);

		// Hand it back under the lock, if it's still wanted: a palette change meanwhile empties
		// _upscaleQueued (see upscaled).
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		if (_upscaleQueued.count(job.key)) _upscaleDone[job.key] = std::move(result); // (dropped if the palette changed meanwhile)
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
