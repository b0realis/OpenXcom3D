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
 * FirstPersonVr.cpp: the calls an OpenXR shell makes (see FirstPersonVr.h), handed to the
 * battlescape's first-person mode showing now (FirstPersonMode::vrCurrent); with none, or VR mode
 * off, they do nothing.
 */
#include "FirstPersonVr.h"
#include <algorithm>
#include <cmath>
#include "FirstPersonMode.h"
#include "../Engine/Options.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// The eyes' pictures for the setting "VR eye picture" (0 to 4), in pixels, each shaped as a Quest 3 eye's field of view,
/// so its pixels stay square: the study's three sizes for the Quest 3's CPU (about 2.8, 3.6 and 5.6 pixels a degree), and
/// two for the graphics card (setting "Renderer": GPU), 8.3 pixels a degree and the headset's own recommended height.
const int VrEyeSizeCount = 5;
const int VrEyeSizes[VrEyeSizeCount][2] = { { 360, 390 }, { 460, 500 }, { 710, 770 }, { 1050, 1140 }, { 1620, 1760 } };

/// The mode to hand a call to: the one showing now, with VR mode on; else null.
FirstPersonMode *modeForVr()
{
	return Options::fpsVrMode > 0 ? FirstPersonMode::vrCurrent() : nullptr;
}

}

/**
 * Is the setting "VR mode" on (On, or with the desktop's Preview)?
 */
bool vrMode()
{
	return Options::fpsVrMode > 0;
}

/**
 * Does a battle show its 3D view now, with VR mode on?
 */
bool vrStereoReady()
{
	FirstPersonMode *mode = modeForVr();
	return mode && mode->vrStereoReady();
}

/**
 * Is the player in the 3D view, with VR mode on (see the header)?
 */
bool vrInThreeD()
{
	FirstPersonMode *mode = modeForVr();
	return mode && mode->vrInThreeD();
}

/**
 * The head's pose this frame, from the shell.
 * @param head The pose: the VIEW space located in the shell's LOCAL space.
 */
void vrSetHeadPose(const VrPose &head)
{
	if (FirstPersonMode *mode = modeForVr())
	{
		mode->vrSetHeadPose(head);
	}
}

/**
 * A head's orientation from angles (see the header): turned about y (right is clockwise seen from above: a negative
 * turn), then up about x, then tilted about z (the look's own axis), in OpenXR's space (x right, y up, z back).
 * @param yawRightDegrees How far the head turned right.
 * @param pitchUpDegrees How far it looks up.
 * @param rollLeftDegrees How far it tilts to the left.
 * @param orientation Set to the unit quaternion (x, y, z, w).
 */
void vrHeadOrientation(double yawRightDegrees, double pitchUpDegrees, double rollLeftDegrees, float orientation[4])
{
	const double degreesToHalfRadians = 3.14159265358979323846 / 360.0;
	const double yawHalf = -yawRightDegrees * degreesToHalfRadians, pitchHalf = pitchUpDegrees * degreesToHalfRadians, rollHalf = rollLeftDegrees * degreesToHalfRadians;
	const double yaw[4] = { 0.0, std::sin(yawHalf), 0.0, std::cos(yawHalf) };
	const double pitch[4] = { std::sin(pitchHalf), 0.0, 0.0, std::cos(pitchHalf) };
	const double roll[4] = { 0.0, 0.0, std::sin(rollHalf), std::cos(rollHalf) };
	// The products: first turned by b, then by a.
	auto multiply = [](const double a[4], const double b[4], double out[4])
	{
		out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
		out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
		out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
		out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	};
	double yawPitch[4], all[4];
	multiply(yaw, pitch, yawPitch);
	multiply(yawPitch, roll, all);
	for (int part = 0; part < 4; ++part)
	{
		orientation[part] = (float)all[part];
	}
}

/**
 * The head's pose this frame from angles and a place (see vrSetHeadPose).
 * @param yawRightDegrees How far the head turned right.
 * @param pitchUpDegrees How far it looks up.
 * @param rollLeftDegrees How far it tilts to the left.
 * @param position Where it is, metres (x right, y up, z back).
 */
void vrSetHeadAngles(double yawRightDegrees, double pitchUpDegrees, double rollLeftDegrees, const float position[3])
{
	VrPose head;
	std::copy(position, position + 3, head.position);
	vrHeadOrientation(yawRightDegrees, pitchUpDegrees, rollLeftDegrees, head.orientation);
	vrSetHeadPose(head);
}

/**
 * Recentres: the soldier's facing where the head looks now, the soldier's eye where it is.
 */
void vrRecentre()
{
	if (FirstPersonMode *mode = modeForVr())
	{
		mode->vrRecentre();
	}
}

/**
 * Turns the world 45 degrees round the player, the soldier turning after it by the game's rules.
 * @param direction -1 left, 1 right.
 */
void vrSnapTurn(int direction)
{
	if (FirstPersonMode *mode = modeForVr())
	{
		mode->vrSnapTurn(direction);
	}
}

/**
 * Draws both eyes now.
 * @param views The left eye's view and the right eye's.
 * @param width Each picture's width, in pixels.
 * @param height Its height.
 * @param pictures Set to the two pictures.
 * @param hand Where the weapon in hand's space is (the hook for a controller's hand); null: the head's.
 * @return False with nothing to draw (pictures left empty).
 */
bool vrRenderEyes(const VrEyeView views[2], int width, int height, VrPicture pictures[2], const VrPose *hand, int cardSlot, bool cardCheck)
{
	FirstPersonMode *mode = modeForVr();
	if (!mode)
	{
		pictures[0] = pictures[1] = VrPicture();
		return false;
	}
	return mode->vrRenderEyes(views, width, height, pictures, hand, cardSlot, cardCheck);
}

/**
 * The HUD as its own picture.
 * @param picture Set to it.
 * @return False with nothing to show.
 */
bool vrHudPicture(VrPicture &picture)
{
	FirstPersonMode *mode = modeForVr();
	if (!mode)
	{
		picture = VrPicture();
		return false;
	}
	return mode->vrHudPicture(picture);
}

/**
 * The controllers this frame (see the header).
 * @param rightAim The right hand's aim pose, or null.
 * @param rightGrip Its grip pose, or null.
 * @param leftAim The left hand's aim pose, or null.
 * @param leftGripHeld The left grip held: two-handed aim.
 */
void vrSetHands(const VrPose *rightAim, const VrPose *rightGrip, const VrPose *leftAim, bool leftGripHeld)
{
	if (FirstPersonMode *mode = modeForVr())
	{
		mode->vrSetHands(rightAim, rightGrip, leftAim, leftGripHeld);
	}
}

/**
 * The right stick's sideways push this frame (see the header).
 * @param axis -1 left .. 1 right.
 */
void vrSetTurnAxis(double axis)
{
	if (FirstPersonMode *mode = modeForVr())
	{
		mode->vrSetTurnAxis(axis);
	}
}

/**
 * The pixels of each eye's picture for a choice of the setting "VR eye picture".
 * @param choice 0 to 4 (others are kept to that).
 * @param width Set to the width.
 * @param height Set to the height.
 */
void vrEyeSize(int choice, int &width, int &height)
{
	const int index = std::clamp(choice, 0, VrEyeSizeCount - 1);
	width = VrEyeSizes[index][0];
	height = VrEyeSizes[index][1];
}

/**
 * The pixels of each eye's picture as the setting has them now.
 * @param width Set to the width.
 * @param height Set to the height.
 */
void vrEyeSize(int &width, int &height)
{
	vrEyeSize(Options::fpsVrEyeSize, width, height);
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
