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
 * FirstPersonViewStereo.cpp: the 3D view for two eyes (VR mode; the Meta Quest 3 port).
 *
 * A stereo frame is the view's own frame cut in two (see draw): its first half, the work every
 * picture of it shares, done once with the view's own camera, the head's middle (the tile
 * snapshot, the colours, the particles moved on, the shots followed: one set of sparks, one
 * static, one moment for both eyes); then its second half once for each eye, with that eye's own
 * picture and per-pixel buffers swapped in for the view's own, its camera where the eye is and
 * looking along its axes, through its off-centre field of view (a headset's eye sees further out
 * than in, and further down than up). The second eye's pass moves nothing on (the units' glides,
 * the flames, the static, the hits watched): the first eye's did, for the frame. The view's
 * threads draw each eye as they draw the view (see runOnAllThreads). Last in each eye, the weapon
 * in hand as a 3D object at its true depth (see setHeldModel, placeHeldModel). A head tilted less
 * than VrLevelRollDegrees is drawn level (see setHeadAxes).
 *
 * Nothing here runs unless VR mode asks for it: with it off, the view draws as it always has.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <SDL.h>
#include "../Engine/Logger.h"
#include "FirstPersonSdl.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/// How often the time the two eyes took is logged while VR draws them (ms): the frame probe the study plans for the
/// headset's log, read on the desktop too.
const Uint32 VrTimingLogTime = 5000;
/// The nearest an aim point may be to an eye and still be put on its picture (voxels ahead): nearer, the crosshair
/// would leave the picture sideways.
const double AimNearest = 0.5;
/// A head tilted less than this many degrees is drawn level (the owner's choice): the units' pictures then take their
/// quick path, row by row (see drawSprite), which only a level view allows.
const double VrLevelRollDegrees = 1.0;
/// Near a wall it's pulled in towards the head's middle (its look from there the same, only nearer), down to this share
/// of that at the most, so it keeps VrHeldWallGap voxels short of the first surface on the lines from there through its
/// corners; nearer still, the wall hides what reaches into it.
const double VrHeldScaleLeast = 0.45, VrHeldWallGap = 1.0;
/// A face of it nearer an eye than this is left out (voxels).
const double VrHeldNearest = 0.2;
/// The weapon's laser (see setLaser): this wide in the world (voxels), at most VrLaserMostPixels across on an eye's
/// picture (and at least one).
const float VrLaserWidth = 0.06f;
const int VrLaserMostPixels = 2;

}

/**
 * Makes an eye's picture and per-pixel buffers for a size (only when it changes: a new picture is
 * a new allocation), and gives the picture the view's palette (it changes with the time of day,
 * night vision, a battle's own colours).
 * @param target The eye's picture and buffers.
 * @param width The picture's width, in pixels.
 * @param height Its height.
 */
void FirstPersonView::makeEyeTarget(EyeTarget &target, int width, int height)
{
	// A new size (or the first): a new 8-bit picture, and its buffers one entry a pixel.
	if (!target.surface || target.width != width || target.height != height)
	{
		std::tie(target.buffer, target.surface) = Surface::NewPair8Bit(width, height);
		target.width = (Uint16)width;
		target.height = (Uint16)height;
		target.pitch = (Uint16)target.surface->pitch;
		const size_t pixels = (size_t)width * height;
		target.depth.assign(pixels, 0.0f);
		target.glow.assign(pixels, 0);
		target.unitVeil.assign(pixels, 0);
		target.smokeVeil.assign(pixels, 0);
		target.ownColours.assign(pixels, 0);
	}
	// The view's palette, as the view's own picture has it now.
	setSurfaceColors(target.surface.get(), getPalette(), 0, 256);
}

/**
 * Swaps an eye's picture and per-pixel buffers in for the view's own (its surface, its size, its
 * depth buffer, glow mask, unit and smoke veils and own colours): everything the view draws then
 * goes into the eye's. Called again, it swaps them back.
 * @param target The eye's picture and buffers.
 */
void FirstPersonView::swapEyeTarget(EyeTarget &target)
{
	std::swap(_alignedBuffer, target.buffer);
	std::swap(_surface, target.surface);
	std::swap(Surface::_width, target.width);
	std::swap(Surface::_height, target.height);
	std::swap(Surface::_pitch, target.pitch); // (the surface's bytes a row: the view's own _pitch is the camera's)
	_depthBuffer.swap(target.depth);
	_glowMask.swap(target.glow);
	_unitVeil.swap(target.unitVeil);
	_smokeVeil.swap(target.smokeVeil);
	_ownColours.swap(target.ownColours);
}

/**
 * VR: the camera looks along a head's own axes (see the header). Its yaw, pitch and tilt, which the
 * rest of the view reads (the units' pictures turned to it, the cards' lean, the sounds' angle),
 * are worked out from them: the yaw where forward points along the ground, the pitch how far it
 * points up, the tilt how far right has turned from level about forward (positive: the head tilted
 * to the left, as setRoll has it); a tilt under VrLevelRollDegrees is drawn level (see
 * levelSmallTilt).
 * @param forward The way it looks (null: back to the yaw and pitch).
 * @param right Its right.
 * @param up Its up.
 */
void FirstPersonView::setHeadAxes(const double forward[3], const double right[3], const double up[3])
{
	_headAxes = forward != nullptr;
	if (!_headAxes)
	{
		_roll = 0.0;
		return;
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		_headForward[axis] = forward[axis];
		_headRight[axis] = right[axis];
		_headUp[axis] = up[axis];
	}
	// The yaw and pitch of forward, and the tilt (a small one made level).
	_yaw = std::atan2(forward[1], forward[0]);
	_pitch = std::asin(std::clamp(forward[2], -1.0, 1.0));
	_roll = levelSmallTilt(_headForward, _headRight, _headUp);
}

/**
 * A head's tilt, and a small one made level (VR, the owner's choice): the angle its right is
 * turned up from the level right for its look (as updateBasis makes that); under
 * VrLevelRollDegrees, its right and up are put level (as level as the look allows) and the tilt is
 * none, so the world, the units' pictures (their quick path, row by row, which only a level view
 * takes: see drawSprite) and the weapon in hand are drawn level.
 * @param forward Where the head looks (a unit vector, true proportions).
 * @param right Its right; made level under the threshold.
 * @param up Its up; made level under the threshold.
 * @return The tilt, radians (0 when made level).
 */
double FirstPersonView::levelSmallTilt(const double forward[3], double right[3], double up[3])
{
	const double yaw = std::atan2(forward[1], forward[0]);
	const double levelRight[3] = { -std::sin(yaw), std::cos(yaw), 0.0 };
	const double levelUp[3] = { -forward[2] * levelRight[1], forward[2] * levelRight[0], forward[0] * levelRight[1] - forward[1] * levelRight[0] };
	const double alongLevelRight = right[0] * levelRight[0] + right[1] * levelRight[1] + right[2] * levelRight[2];
	const double alongLevelUp = right[0] * levelUp[0] + right[1] * levelUp[1] + right[2] * levelUp[2];
	const double tilt = std::atan2(alongLevelUp, alongLevelRight);
	if (std::abs(tilt) >= VrLevelRollDegrees * Pi / 180.0)
	{
		return tilt;
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		right[axis] = levelRight[axis];
		up[axis] = levelUp[axis];
	}
	return 0.0;
}

/**
 * Points the camera as an eye looks, its picture swapped in (see renderEyes): the eye's place, its
 * axes (see setHeadAxes), and its field of view on the picture. The pixels stay square: the
 * picture shows the field of view asked for, widened about its middle on whichever axis the
 * picture's shape has room to spare (a picture shaped as the field of view, as OpenXR's
 * recommended sizes are, shows it as it is). Where the view axis meets the picture follows: off
 * the middle as the field of view is off centre. Notes the field of view drawn on the target.
 * @param eye The eye.
 * @param target Its picture and buffers (swapped in: the view's size is the picture's now).
 */
void FirstPersonView::useEyeCamera(const EyeCamera &eye, EyeTarget &target)
{
	// The place and the axes.
	for (int axis = 0; axis < 3; ++axis)
	{
		_eye[axis] = eye.eye[axis];
	}
	setHeadAxes(eye.forward, eye.right, eye.up);
	updateBasis();
	// The field of view: pixels per unit of the image plane (the same across and up), the middle of the field asked
	// for, and so where the view axis (the plane's 0, 0) lands on the picture.
	const double width = getWidth(), height = getHeight();
	const double spanAcross = std::max(1e-6, eye.tangentRight - eye.tangentLeft), spanUp = std::max(1e-6, eye.tangentUp - eye.tangentDown);
	_eyeFocal = std::min(width / spanAcross, height / spanUp);
	const double middleAcross = (eye.tangentLeft + eye.tangentRight) * 0.5, middleUp = (eye.tangentUp + eye.tangentDown) * 0.5;
	_eyeAxisX = width * 0.5 - middleAcross * _eyeFocal;
	_eyeAxisY = height * 0.5 + middleUp * _eyeFocal;
	_eyePass = true;
	// The field of view the picture shows (left, right, up, down), for the shell's projection layer.
	target.tangents[0] = middleAcross - width * 0.5 / _eyeFocal;
	target.tangents[1] = middleAcross + width * 0.5 / _eyeFocal;
	target.tangents[2] = middleUp + height * 0.5 / _eyeFocal;
	target.tangents[3] = middleUp - height * 0.5 / _eyeFocal;
}

/**
 * Draws the view for two eyes (VR mode; see the header and this file's top). The view's own
 * camera is the head's middle (the mode sets it, see FirstPersonMode::vrApplyHead): the frame's
 * shared work is done with it, once. Then each eye: its picture swapped in, its camera, the
 * pointer (for the unit pointed at, the downed unit's icons) where the aim point is on its
 * picture, and the frame's second half; the second eye's moving nothing on. The view's own
 * camera is put back after, and its own picture is left as it was (not drawn this frame). Last in
 * each eye, the weapon in hand (see setHeldModel), put into the world once (see placeHeldModel).
 * The time it all took is summed, and logged every VrTimingLogTime.
 * @param eyes The two eyes: the left, the right.
 * @param width Each eye's picture's width, in pixels.
 * @param height Its height.
 * @param aimPoint The point the crosshair is on, in voxels (null: none).
 * @param hand Where the hand's space is in the world, for the weapon in hand (null: none drawn).
 */
void FirstPersonView::renderEyes(const EyeCamera eyes[2], int width, int height, const double aimPoint[3], const HeldFrame *hand)
{
	// Each eye's picture, at this size, in the view's palette.
	for (EyeTarget &target : _eyeTargets)
	{
		makeEyeTarget(target, width, height);
	}
	// Nothing to look through yet: blank pictures.
	if (!_save || !_cameraUnit)
	{
		for (EyeTarget &target : _eyeTargets)
		{
			std::fill((Uint8 *)target.surface->pixels, (Uint8 *)target.surface->pixels + (size_t)target.pitch * target.height, (Uint8)0);
			target.aimShown = false;
		}
		return;
	}
	const auto started = std::chrono::steady_clock::now();
	_redraw = false; // (the view's own picture isn't drawn this frame: the eyes are)

	// The frame's shared work, with the head's camera, the weapon in hand put into the world in it (see beginFrame).
	_heldWorldQuads.clear();
	_heldWorldFlashes.clear();
	_heldMuzzleWorldShown[0] = _heldMuzzleWorldShown[1] = false;
	_heldFrame = hand;
	beginFrame();
	_heldFrame = nullptr;
	const auto shared = std::chrono::steady_clock::now();

	// The head's camera, to put back after the eyes: its place, its way of looking, the pointer, and the muzzles of the
	// weapons in hand (on the desktop's picture, not the eyes': a straight path's laser starts from the path itself).
	const double headEye[3] = { _eye[0], _eye[1], _eye[2] };
	const double headYaw = _yaw, headPitch = _pitch, headRoll = _roll;
	const bool headAxes = _headAxes;
	const double headForward[3] = { _headForward[0], _headForward[1], _headForward[2] };
	const double headRight[3] = { _headRight[0], _headRight[1], _headRight[2] };
	const double headUp[3] = { _headUp[0], _headUp[1], _headUp[2] };
	const int pointerX = _pointerX, pointerY = _pointerY;
	const bool muzzleValid = _muzzleValid, offHandMuzzleValid = _offHandMuzzleValid;
	_muzzleValid = _offHandMuzzleValid = false;
	std::copy(headEye, headEye + 3, _spriteEye);

	// Each eye's picture: its target swapped in, its camera, the pointer where the aim point is on it, and the frame's
	// second half (the second eye's moving nothing on).
	for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex)
	{
		EyeTarget &target = _eyeTargets[eyeIndex];
		swapEyeTarget(target);
		useEyeCamera(eyes[eyeIndex], target);
		target.aimShown = false;
		_pointerX = _pointerY = -(1 << 20); // (none: the pointer's patch round it finds nothing)
		if (aimPoint)
		{
			// The aim point in the eye's frame (true proportions), then on its picture, however near.
			double ahead = 0.0, toRight = 0.0, upward = 0.0;
			for (int axis = 0; axis < 3; ++axis)
			{
				const double relative = (aimPoint[axis] - _eye[axis]) * (axis == 2 ? ZScale : 1.0);
				ahead += relative * _forward[axis];
				toRight += relative * _right[axis];
				upward += relative * _up[axis];
			}
			if (ahead > AimNearest)
			{
				target.aim[0] = _eyeAxisX + toRight * _eyeFocal / ahead;
				target.aim[1] = _eyeAxisY - upward * _eyeFocal / ahead;
				target.aimShown = true;
				_pointerX = (int)std::floor(target.aim[0]);
				_pointerY = (int)std::floor(target.aim[1]);
			}
		}
		_replayPass = eyeIndex == 1;
		drawPass();
		drawFacing();
		drawHeldModel(); // (the weapon in hand last, as the desktop draws it over the view)
		drawLaser();
		unlock();
		swapEyeTarget(target);
	}

	// The head's camera back, as the mode set it.
	_replayPass = false;
	_eyePass = false;
	std::copy(headEye, headEye + 3, _eye);
	_yaw = headYaw;
	_pitch = headPitch;
	_roll = headRoll;
	_headAxes = headAxes;
	std::copy(headForward, headForward + 3, _headForward);
	std::copy(headRight, headRight + 3, _headRight);
	std::copy(headUp, headUp + 3, _headUp);
	updateBasis();
	_pointerX = pointerX;
	_pointerY = pointerY;
	_muzzleValid = muzzleValid;
	_offHandMuzzleValid = offHandMuzzleValid;
	_heldMuzzleWorldShown[0] = _heldMuzzleWorldShown[1] = false; // (the desktop's shots leave the muzzle on its picture)
	_laserShown = false; // (set again before the next)

	// The time it took: summed, and every VrTimingLogTime logged (the average frame, its shared part and each eye's,
	// and the slowest frame).
	const auto finished = std::chrono::steady_clock::now();
	const double totalMs = std::chrono::duration<double, std::milli>(finished - started).count();
	const double sharedMs = std::chrono::duration<double, std::milli>(shared - started).count();
	const Uint32 now = SDL_GetTicks();
	if (!_stereoTimingSince)
	{
		_stereoTimingSince = now;
	}
	++_stereoFrames;
	_stereoTotal += totalMs;
	_stereoShared += sharedMs;
	_stereoWorst = std::max(_stereoWorst, totalMs);
	if (now - _stereoTimingSince >= VrTimingLogTime)
	{
		const double frames = _stereoFrames;
		Log(LOG_INFO) << "OpenXcom3D: VR, two eyes of " << width << "x" << height << ": " << std::fixed << std::setprecision(2) << _stereoTotal / frames
			<< " ms a frame (the shared part " << _stereoShared / frames << ", each eye " << (_stereoTotal - _stereoShared) / frames / 2.0
			<< "), the slowest " << _stereoWorst << " ms, over " << _stereoFrames << " frames";
		_stereoTimingSince = now;
		_stereoFrames = 0;
		_stereoTotal = _stereoShared = _stereoWorst = 0.0;
	}
}

/**
 * Puts the weapon in hand into the world for this frame (renderEyes's beginFrame, once the tiles
 * are snapshot and before the shots are followed, so that a shot of ours starting now leaves its
 * muzzle: see beginShot): from the hand's space, VrHeldScale voxels to its unit. Near a wall it's
 * pulled in towards the hand's origin (the head's middle: its look from there stays the same, it
 * only comes nearer): on the lines from the origin through the corners and the middle of the box
 * round it, the first surface the view draws keeps it VrHeldWallGap short, down to
 * VrHeldScaleLeast at the most; nearer than that, the eyes' depth test hides what reaches into
 * the wall. The rays walk the voxel grid, where a voxel up is ZScale of one across.
 * @param frame Where the hand's space is in the world.
 */
void FirstPersonView::placeHeldModel(const HeldFrame &frame)
{
	_heldWorldQuads.clear();
	_heldWorldFlashes.clear();
	_heldMuzzleWorldShown[0] = _heldMuzzleWorldShown[1] = false;
	if (_heldModel.quads.empty())
	{
		return;
	}
	// A point (or a direction) of the hand's space as an offset in the world, true proportions, a unit to a voxel across.
	auto worldOffset = [&frame](const float point[3], double out[3])
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			out[axis] = point[0] * frame.right[axis] + point[1] * frame.down[axis] + point[2] * frame.ahead[axis];
		}
	};
	// The box round its faces, in the hand's space.
	float low[3] = { 1e9f, 1e9f, 1e9f }, high[3] = { -1e9f, -1e9f, -1e9f };
	for (const FirstPersonHeldQuad &quad : _heldModel.quads)
	{
		for (const auto &corner : quad.corners)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				low[axis] = std::min(low[axis], corner[axis]);
				high[axis] = std::max(high[axis], corner[axis]);
			}
		}
	}
	// The pull towards the origin: each of the box's corners and its middle along its line from the origin (in the voxel
	// grid), against the first surface on that line. (A hand's frame: none, its weapon slid back already.)
	double scale = VrHeldScale;
	const float origin[3] = { (float)frame.origin[0], (float)frame.origin[1], (float)frame.origin[2] };
	for (int sample = 0; sample < (frame.pullIn ? 9 : 0); ++sample)
	{
		const float point[3] = { sample == 8 ? (low[0] + high[0]) * 0.5f : (sample & 1) ? high[0] : low[0],
			sample == 8 ? (low[1] + high[1]) * 0.5f : (sample & 2) ? high[1] : low[1],
			sample == 8 ? (low[2] + high[2]) * 0.5f : (sample & 4) ? high[2] : low[2] };
		double offset[3];
		worldOffset(point, offset);
		offset[2] /= ZScale; // (into the voxel grid)
		const double length = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
		if (length < 1e-6)
		{
			continue;
		}
		const float direction[3] = { (float)(offset[0] / length), (float)(offset[1] / length), (float)(offset[2] / length) };
		float surface[3];
		if (visibleSurfaceAlong(origin, direction, (float)(VrHeldScale * length + VrHeldWallGap), surface))
		{
			const double reach = std::sqrt((double)(surface[0] - origin[0]) * (surface[0] - origin[0]) + (double)(surface[1] - origin[1]) * (surface[1] - origin[1])
				+ (double)(surface[2] - origin[2]) * (surface[2] - origin[2]));
			scale = std::min(scale, (reach - VrHeldWallGap) / length);
		}
	}
	scale = std::max(scale, VrHeldScaleLeast * VrHeldScale);
	// Its faces, flashes and muzzles in the world, in voxels (a voxel up being ZScale of one across).
	auto toWorld = [&](const float point[3], double out[3])
	{
		double offset[3];
		worldOffset(point, offset);
		for (int axis = 0; axis < 3; ++axis)
		{
			out[axis] = frame.origin[axis] + offset[axis] * scale / (axis == 2 ? ZScale : 1.0);
		}
	};
	_heldWorldQuads.resize(_heldModel.quads.size());
	for (size_t index = 0; index < _heldModel.quads.size(); ++index)
	{
		const FirstPersonHeldQuad &quad = _heldModel.quads[index];
		HeldWorldQuad &placed = _heldWorldQuads[index];
		for (int corner = 0; corner < 4; ++corner)
		{
			toWorld(quad.corners[corner], placed.corners[corner]);
		}
		worldOffset(quad.normal, placed.normal);
		placed.color = quad.color;
	}
	for (const FirstPersonHeldFlash &flash : _heldModel.flashes)
	{
		HeldWorldFlash placed;
		toWorld(flash.middle, placed.middle);
		placed.radius = flash.radius * scale;
		placed.look = flash;
		_heldWorldFlashes.push_back(placed);
	}
	for (int gun = 0; gun < 2; ++gun)
	{
		if (_heldModel.muzzleShown[gun])
		{
			double muzzle[3];
			toWorld(_heldModel.muzzles[gun], muzzle);
			for (int axis = 0; axis < 3; ++axis)
			{
				_heldMuzzleWorld[gun][axis] = (float)muzzle[axis];
			}
			_heldMuzzleWorldShown[gun] = true;
		}
	}
}

/**
 * Draws the weapon in hand in an eye's picture (renderEyes, after the frame's last marks, as the
 * desktop draws it over the view): the faces turned to this eye, far to near (the desktop's
 * painter's order), each at its depth, hidden where the world is nearer (a wall it reaches into);
 * a face reaching nearer the eye than VrHeldNearest is left out. Then its muzzle flashes, as the
 * desktop draws them (a disc with turning rays, in its core, middle and rim colours), as big as
 * they are in the world, behind the weapon.
 */
void FirstPersonView::drawHeldModel()
{
	if (_heldWorldQuads.empty())
	{
		return;
	}
	const double focal = focalPixels(), middleX = axisX(), middleY = axisY();
	// A point of the world relative to the eye, in true proportions.
	auto fromEye = [this](const double world[3], double relative[3])
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			relative[axis] = (world[axis] - _eye[axis]) * (axis == 2 ? ZScale : 1.0);
		}
	};
	// ...on the picture, with its depth along the eye's view axis (false: too near, or behind).
	auto project = [&](const double relative[3], double screen[2], double &depth)
	{
		depth = relative[0] * _forward[0] + relative[1] * _forward[1] + relative[2] * _forward[2];
		if (depth < VrHeldNearest)
		{
			return false;
		}
		const double toRight = relative[0] * _right[0] + relative[1] * _right[1] + relative[2] * _right[2];
		const double upward = relative[0] * _up[0] + relative[1] * _up[1] + relative[2] * _up[2];
		screen[0] = middleX + toRight * focal / depth;
		screen[1] = middleY - upward * focal / depth;
		return true;
	};
	// The faces turned to this eye, on its picture.
	struct Face { double screen[4][2]; double depth, distance; Uint8 color; };
	std::vector<Face> faces;
	faces.reserve(_heldWorldQuads.size() / 2);
	for (const HeldWorldQuad &quad : _heldWorldQuads)
	{
		double centre[3] = { 0.0, 0.0, 0.0 }, corners[4][3];
		for (int corner = 0; corner < 4; ++corner)
		{
			fromEye(quad.corners[corner], corners[corner]);
			for (int axis = 0; axis < 3; ++axis) centre[axis] += corners[corner][axis] * 0.25;
		}
		if (quad.normal[0] * centre[0] + quad.normal[1] * centre[1] + quad.normal[2] * centre[2] >= 0.0)
		{
			continue; // (turned away from this eye)
		}
		Face face;
		bool shown = true;
		double depthSum = 0.0;
		for (int corner = 0; corner < 4 && shown; ++corner)
		{
			double depth;
			shown = project(corners[corner], face.screen[corner], depth);
			depthSum += depth;
		}
		if (!shown)
		{
			continue;
		}
		face.depth = depthSum * 0.25;
		face.distance = centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2];
		face.color = quad.color;
		faces.push_back(face);
	}
	std::sort(faces.begin(), faces.end(), [](const Face &first, const Face &second) { return first.distance > second.distance; });
	for (const Face &face : faces)
	{
		fillQuadAtDepth(face.screen, face.color, (float)face.depth);
	}
	// The muzzle flashes: the desktop's disc and rays, each pixel behind whatever is nearer (the muzzle covers the back
	// of it).
	const int width = getWidth(), height = getHeight();
	for (const HeldWorldFlash &flash : _heldWorldFlashes)
	{
		double relative[3], screen[2], depth;
		fromEye(flash.middle, relative);
		if (!project(relative, screen, depth))
		{
			continue;
		}
		const FirstPersonHeldFlash &look = flash.look;
		const double radius = std::max(1.0, flash.radius * focal / depth);
		const int reach = (int)std::ceil(radius * (1.0 + look.spike));
		const int flashX = (int)std::lround(screen[0]), flashY = (int)std::lround(screen[1]);
		for (int offsetY = -reach; offsetY <= reach; ++offsetY)
		{
			for (int offsetX = -reach; offsetX <= reach; ++offsetX)
			{
				const int pixelX = flashX + offsetX, pixelY = flashY + offsetY;
				if (pixelX < 0 || pixelY < 0 || pixelX >= width || pixelY >= height || depth >= _depthBuffer[pixelY * width + pixelX]) continue;
				const double distance = std::sqrt((double)(offsetX * offsetX + offsetY * offsetY)), angle = std::atan2((double)offsetY, (double)offsetX);
				const double rayStrength = std::pow(std::max(0.0, std::cos(angle * look.rays + look.turn)), (double)look.sharpness);
				const double edgeRadius = radius * (1.0 + look.spike * rayStrength);
				if (distance > edgeRadius) continue;
				*getRaw(pixelX, pixelY) = distance < edgeRadius * 0.35 ? look.core : distance < edgeRadius * 0.7 ? look.mid : look.rim;
			}
		}
	}
}

/**
 * VR: the weapon's laser for the next renderEyes (setting "VR laser"): a thin beam from its muzzle
 * to where it points, drawn in each eye at its depth, the game's laser sight's red, or dimmed (dark
 * red) where the soldier can't fire.
 * @param from The muzzle, in voxels.
 * @param to Where it points, in voxels.
 * @param dimmed Dimmed: the soldier can't fire there.
 */
void FirstPersonView::setLaser(const double from[3], const double to[3], bool dimmed)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		_laserFrom[axis] = (float)from[axis];
		_laserTo[axis] = (float)to[axis];
	}
	_laserDimmed = dimmed;
	_laserShown = true;
}

/**
 * Draws the weapon's laser in an eye's picture (see setLaser): VrLaserWidth wide in the world, at
 * least a pixel and at most VrLaserMostPixels, behind whatever is nearer (the weapon's own faces
 * don't hide its start: it leaves the muzzle).
 */
void FirstPersonView::drawLaser()
{
	if (!_laserShown)
	{
		return;
	}
	drawLine3D(_laserFrom, _laserTo, _laserDimmed ? _colors.laserRefused : _colors.laserSight, VrLaserMostPixels, 1.0f, VrLaserWidth, 0, getHeight(),
		!_laserDimmed, false);
}

/**
 * An eye's picture from the last renderEyes: its pixels (in the view's palette), its size, the
 * field of view it shows, where the aim point is on it, and its depth buffer (the world's, the
 * units' and the weapon in hand's depths, as the view draws them: for the shell's depth layer).
 * @param eye 0 the left eye, 1 the right.
 * @return The picture (no pixels before the first renderEyes).
 */
FirstPersonView::EyePicture FirstPersonView::eyePicture(int eye)
{
	EyePicture picture;
	EyeTarget &target = _eyeTargets[std::clamp(eye, 0, 1)];
	if (!target.surface)
	{
		return picture;
	}
	picture.pixels = (Uint8 *)target.surface->pixels;
	picture.width = target.width;
	picture.height = target.height;
	picture.pitch = target.pitch;
	picture.tangentLeft = target.tangents[0];
	picture.tangentRight = target.tangents[1];
	picture.tangentUp = target.tangents[2];
	picture.tangentDown = target.tangents[3];
	picture.aimX = target.aim[0];
	picture.aimY = target.aim[1];
	picture.aimShown = target.aimShown;
	picture.depth = target.depth.size() >= (size_t)target.width * target.height ? target.depth.data() : nullptr;
	return picture;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
