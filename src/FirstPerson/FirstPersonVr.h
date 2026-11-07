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
 * FirstPersonVr.h: the 3D view in VR, as an OpenXR app shell sees it (the Meta Quest 3 port; see
 * the study quest3_vr_study.md, sections 3 and 4). The shell owns the OpenXR session, its layers
 * and the controllers; this is what it calls, on the game's thread, once a frame, after the
 * game's own frame (the battlescape's think and blit; the shell's hook is in Screen::flip):
 *
 *   1. vrSetHeadPose: the head's pose (the VIEW space located in the shell's LOCAL space at the
 *      frame's predicted display time; or vrSetHeadAngles, the same as angles). The 3D view looks
 *      with it, anywhere; the head never turns the soldier (the owner, after QuestZDoom: "doom2 on
 *      quest looks around with head").
 *   2. vrSetHands: the controllers' poses (the right hand's aim and grip, the left hand's aim, and
 *      whether the left grip is held), the same space and time. The right hand holds the weapon:
 *      drawn in it, aiming and firing from its muzzle along its barrel, the crosshair (and the
 *      laser, setting "VR laser") where that line meets something; the left grip held, the barrel
 *      points from the right hand through the left. The hands never turn the soldier either.
 *   3. vrSetTurnAxis: the right stick's sideways push. Only it turns the soldier (setting "VR
 *      turning": smooth, as far as it's pushed, or snap, 45 degrees a push): the world turns round
 *      the player, and the game's eight facings change as his way crosses each sector, each change
 *      paying the game's turning time units (too few: he stops turning, the game's warning says
 *      why). The left stick (the game's movement keys) walks and strafes along his way.
 *   4. vrRenderEyes: both eyes' pictures, from xrLocateViews's two views (each eye's pose in the
 *      same space, and its four angles), at the swapchains' size, for the poses just set: the eye
 *      moved with the head, the weapon in the hand, its laser and crosshair found for it there, so
 *      nothing lags by a frame. 8-bit pictures, with the palette to colour them (the shell uploads
 *      them through it, as the study plans it), and each one's depth.
 *   5. vrHudPicture: the HUD (bars, panel, compass, texts) as its own picture, palette index 0
 *      transparent, for a quad layer about VrHudWidthDegrees wide, fixed where the head looks (no
 *      crosshair: that's in the eyes, at the depth of what the weapon points at).
 *   The game's rules see the poses in the next frame's think (a frame later makes no difference to
 *   them). Also vrInThreeD (immersive or the pane), vrRecentre (a reference space change, the long
 *   press of the Meta button) and vrSnapTurn (a snap turn by hand).
 *
 * Everything here does nothing (and answers false) while the setting "VR mode" is off, or no
 * battle shows its 3D view. The desktop has the same path with a stand-in head (the mouse) and
 * the setting's "Preview": both eyes side by side (see FirstPersonModeVr.cpp).
 *
 * Space: OpenXR's, metres, x right, y up, z back (a head looking ahead looks down -z). The world:
 * voxels, x east, y south, z up; a voxel across is VrMetresPerVoxel (10 cm: a tile 1.6 m, a soldier
 * 1.8 m tall), a voxel up ZScale of that (the isometric art's flattened height).
 */
#include <SDL.h>

namespace OpenXcom
{

namespace FirstPerson
{

/// A pose, as OpenXR's XrPosef: where (metres, x right, y up, z back) and which way (a unit quaternion x, y, z, w).
struct VrPose
{
	float position[3];
	float orientation[4];
};
/// An eye's field of view, as OpenXR's XrFovf: the angles of its four edges from the eye's view axis, in radians (left
/// and down negative); a headset's are off centre (a Quest 3's left eye: 54 degrees left, 40 right, 44 up, 55 down).
struct VrFov
{
	float angleLeft, angleRight, angleUp, angleDown;
};
/// One eye to draw, as OpenXR's XrView from xrLocateViews: its pose (in the same space as the head's) and its field of view.
struct VrEyeView
{
	VrPose pose;
	VrFov fov;
};
/// A picture handed to the shell: 8-bit palette indices row by row (pitch bytes a row), and the 256 colours to show
/// them in; for an eye, the field of view it shows (what the shell puts in the projection layer: the one asked for,
/// widened on one axis when the picture's shape asks for square pixels), and its depth: for each pixel how far ahead
/// of the eye (along its view axis, -z) what it shows is, row by row (depthPitch floats a row), in voxels across:
/// times depthToMetres for metres; the sky +infinity (for XR_KHR_composition_layer_depth; the HUD has none). The
/// pixels and the depth stay valid until the next call that draws them.
struct VrPicture
{
	const Uint8 *pixels = nullptr;
	int width = 0, height = 0, pitch = 0;
	const SDL_Color *palette = nullptr;
	VrFov fov = {};
	const float *depth = nullptr;
	int depthPitch = 0;
	float depthToMetres = 0.0f;
};

/// How big a voxel across is, in metres (10 cm: a tile 16 of them, 1.6 m; a soldier 22 voxels up, 1.8 m tall).
const double VrMetresPerVoxel = 0.1;
/// The HUD's quad: how wide it looks, in degrees, and how far in front of the head it hangs, in metres (the desktop
/// preview draws it into each eye there; the shell may place its quad layer the same way).
const double VrHudWidthDegrees = 60.0, VrHudDistance = 1.5;

/// Is the setting "VR mode" on (On, or with the desktop's Preview)?
bool vrMode();
/// Does a battle show its 3D view now, with VR mode on: is there anything for vrRenderEyes to draw?
bool vrStereoReady();
/// Is the player in the 3D view, with VR mode on: from F8 in to F8 out on the player's turns, also while a 2D screen is
/// over it (inventory, a message, the options) and for moments with no soldier to look through? The shell stays
/// immersive while it holds. The aliens' turn always plays on the isometric map with VR mode on (the owner: too much
/// switching of camera), so it drops when the player's turn ends, and comes back by itself with the player's next turn
/// if he was in 3D (as the desktop comes back to 3D); also aiming on the map (a launcher's waypoints) drops it.
bool vrInThreeD();
/// The head's pose this frame, from the shell (from now on the desktop's stand-in, the mouse, no longer turns it).
void vrSetHeadPose(const VrPose &head);
/// The same from angles: the head turned right, looking up and tilted to the left by so many degrees (in that order, as
/// a head turns), and where it is (metres, x right, y up, z back).
void vrSetHeadAngles(double yawRightDegrees, double pitchUpDegrees, double rollLeftDegrees, const float position[3]);
/// A head's orientation as a unit quaternion (x, y, z, w) from those angles (the desktop's stand-in head makes its own so).
void vrHeadOrientation(double yawRightDegrees, double pitchUpDegrees, double rollLeftDegrees, float orientation[4]);
/// The controllers this frame (LOCAL space, the same predicted time as the head's; null for one not tracked): the right
/// hand's aim pose (its -z the way it points) and grip pose (where the hand holds the weapon), the left hand's aim pose,
/// and whether the left grip is held (two-handed aim: the barrel from the right hand through the left). Before
/// vrRenderEyes. With no right hand the weapon is held where the flat view has it, aiming where the head looks.
void vrSetHands(const VrPose *rightAim, const VrPose *rightGrip, const VrPose *leftAim, bool leftGripHeld);
/// The right stick's sideways push this frame, -1 (left) to 1 (right). Smooth turning turns as far as it's pushed;
/// snap turning snaps once each time it's pushed past half-way. Once this is called, the turn keys no longer turn.
void vrSetTurnAxis(double axis);
/// Recentres: the head's look now becomes the soldier's facing, the soldier's eye where the head is now.
void vrRecentre();
/// Turns the soldier 45 degrees (-1 left, 1 right) with the world round the player, at the game's turning cost.
void vrSnapTurn(int direction);
/// Draws both eyes now (on the game's thread, after its frame): views[0] the left eye, views[1] the right, each picture
/// width x height pixels: the weapon in the right hand (setting "VR weapon"), its laser and the crosshair where it points,
/// each at its depth. False (pictures left empty) with nothing to draw. (hand, the earlier hook: a right hand's pose for
/// this frame, taken as its aim and grip when vrSetHands gave none; null: none.)
bool vrRenderEyes(const VrEyeView views[2], int width, int height, VrPicture pictures[2], const VrPose *hand = nullptr);
/// The HUD as its own picture (index 0 transparent), as the last frame left it; false with nothing to show. It never has
/// the weapon in hand (that's in the eyes, or with "VR weapon" off nowhere).
bool vrHudPicture(VrPicture &picture);
/// The pixels of each eye's picture for a choice of the setting "VR eye picture" (0 to 2).
void vrEyeSize(int choice, int &width, int &height);
/// The pixels of each eye's picture as the setting has them now.
void vrEyeSize(int &width, int &height);

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
