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
 *  Date: 2026-11-06
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
 * FirstPersonXrSmoothing.h: VR mode's eye pictures smoothed by the headset's graphics chip (the
 * Meta Quest build; also built by quest3's offline comparison tool). The 3D view draws each eye
 * on the CPU, its pixels big squares in the headset (the owner: "the blocky look is hard on the
 * eyes"); with the setting "VR smoothing" each picture is drawn SmoothingScale times bigger into
 * its eye's swapchain image by an edge-smoothing filter, at almost no cost to the CPU, and its
 * depth with it (each depth value over its block, so the compositor sees the same depth at the
 * same field of view). Two filters: AMD FidelityFX FSR 1 (EASU, edge-adaptive upscaling, then
 * RCAS, sharpening; MIT licence) and SABR v3.0 (an xBR-style pixel-art scaler; GPL 2 or later).
 * See FirstPersonXrSmoothing.cpp.
 */
#if defined(__ANDROID__) || defined(OPENXCOM3D_SMOOTHING_TOOL)
#include <cstdint>
#include <string>

namespace OpenXcom
{

namespace FirstPerson
{

namespace Xr
{

/// How many times bigger a smoothed eye picture is drawn, and the most pixels it may have on a side (a bigger eye
/// picture is scaled less: 710x770 twice).
const int SmoothingScale = 3, SmoothingMaxSize = 2200;
/// The scale for an eye picture of this size (see SmoothingScale).
int smoothingScaleFor(int width, int height);

/// The smoothing filters (see the header's comment).
enum class SmoothingFilter
{
	Fsr, Sabr
};

/**
 * Draws pictures bigger with a smoothing filter, on the current OpenGL ES 3 context: the eye's
 * picture (RGBA, the bottom row first) into a colour texture (an eye's swapchain image), its depth
 * into a depth texture.
 */
class EyeSmoother
{
public:
	/// Makes the filter's programs and textures on the current context; false (and why in error) if it can't.
	bool create(SmoothingFilter filter, std::string &error);
	/// Deletes them (on the same context).
	void destroy();
	/// Is it made?
	bool ready() const { return _ready; }
	/// The filter it was made with.
	SmoothingFilter filter() const { return _filter; }
	/// Draws a picture (RGBA bytes, the bottom row first) into a colour texture of the target size; srgbTarget: the
	/// texture is sRGB (the filter's result is written as linear light, which the chip encodes back). False on an error.
	bool drawColour(const uint32_t *pixels, int width, int height, unsigned int target, int targetWidth, int targetHeight, bool srgbTarget);
	/// Draws a depth picture (0 to 1, the bottom row first) into a depth texture of the target size, each value over its
	/// block. False on an error.
	bool drawDepth(const float *depth, int width, int height, unsigned int target, int targetWidth, int targetHeight);

private:
	bool _ready = false;
	SmoothingFilter _filter = SmoothingFilter::Fsr;
	unsigned int _framebuffer = 0, _vertexArray = 0;
	unsigned int _source = 0, _middle = 0, _depthSource = 0; // the picture's texture, FSR's middle picture, the depth's
	int _sourceWidth = 0, _sourceHeight = 0, _middleWidth = 0, _middleHeight = 0, _depthWidth = 0, _depthHeight = 0;
	unsigned int _firstPass = 0, _secondPass = 0, _depthPass = 0; // the programs: FSR's EASU and RCAS (SABR: first only), depth
};

}

}

}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
