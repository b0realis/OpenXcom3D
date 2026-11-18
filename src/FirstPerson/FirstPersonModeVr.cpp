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
 * FirstPersonModeVr.cpp: VR mode (the setting "VR mode"; the Meta Quest 3 port, see the study
 * quest3_vr_study.md and FirstPersonVr.h).
 *
 * The owner's rules, all under the one setting ("These settings should be all under VR mode
 * on/off!"), as QuestZDoom plays Doom 2 on the Quest ("doom2 on quest looks around with head.
 * Controller moves the weapon and crosshair. We turn and walk only with sticks."):
 * - The head looks: anywhere, always smoothly, never stopped or held, and never turns the soldier.
 * - The right controller holds the weapon (setting "VR weapon": drawn in it): aiming and firing go
 *   from its muzzle along its barrel, to the first thing that line meets (see vrUpdateRifle,
 *   aimRay); there the crosshair, and the laser from the muzzle (setting "VR laser"), each at its
 *   depth. The left grip held, the barrel points from the right hand through the left. The weapon
 *   never turns the soldier either.
 * - Only the right stick turns him (setting "VR turning": smooth, as far as it's pushed, or snap,
 *   45 degrees a push): the world turns round the player, and his way (_vrBodyYaw) with it; the
 *   game's eight facings follow it as it crosses each sector, each change at the game's turning
 *   cost with "Turning costs TU" (too few time units: he stops turning, and the game's warning
 *   says why; see vrFaceBody). A shot needs its target in his front arc, what he sees by the
 *   game's rules: outside it the laser and the crosshair are dimmed and he can't fire there (the
 *   game doesn't turn him for it).
 * - The left stick (the movement keys) walks and strafes along his way, as the setting "Movement"
 *   says (the owner: "Body movement should behave as in tiled movement/or free movement"): tile
 *   moves are quick steps (a tile in a tenth of a second, as the numpad's), free walking slower
 *   (VrFreeWalkSpeed, 1.5 m/s; see FirstPersonModeMovement.cpp).
 * - Comfort (the study, section 4): nothing moves the view but the head and the stick: no screen
 *   shake, no recoil kick, no head bob, no landing dip, no lunge, no tilt from the lean keys, no zoom.
 * - The aliens' turn plays on the isometric map (see alienTurn3D).
 *
 * The head and hands: the OpenXR shell's poses (FirstPerson::vrSetHeadPose, vrSetHands), or on
 * the desktop stand-ins: the look keys turn the head, the lean keys lean it, the mouse aims the
 * right hand, the right button held puts the left hand two-handed where the head looks. The
 * headset's space (metres, x right, y up, z back) turns into the world (voxels, x east, y south,
 * z up) about the yaw the last recentring and the stick's turns set (entering 3D, a cut to another
 * soldier: his facing where the player looks), and the head's move since the recentring moves the
 * eye from the soldier's (as far as a lean reaches, short of walls); the game's lean shots see that
 * move as a lean.
 *
 * The eyes: drawn by the 3D view (FirstPersonView::renderEyes), with the weapon, its laser and the
 * crosshair. The HUD: our overlays as their own picture (vrComposeHud), for the shell's quad layer.
 * The desktop's preview (the setting's third value) shows both eyes side by side with the HUD over
 * each at its depth.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "FirstPersonView.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/TileEngine.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/Surface.h"
#include "../Interface/Text.h"
#include "../Battlescape/WarningMessage.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonModeShared.h"
#include "FirstPersonSdl.h"
#include "../Battlescape/UnitTurnBState.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/Pathfinding.h"

namespace OpenXcom
{

namespace
{

/// A voxel up is this much of a voxel across (the isometric art's flattened height: sqrt(2/3), the 3D view's ZScale).
const double VrVoxelHeight = 0.81649658092772603;
/// The desktop's stand-in head: its eyes this far apart, in metres (a common eye distance; a headset gives its own).
const double VrStandInEyeDistance = 0.063;
/// ...each eye seeing as a Meta Quest 3's does, in degrees from its view axis: left, right, up and down (the left eye's;
/// the right eye's mirrored; hmdgdb's figures, as the study has them).
const double VrStandInFov[2][4] = { { 54.0, 40.0, 44.0, 55.0 }, { 40.0, 54.0, 44.0, 55.0 } };
/// How far the head may move the eye from the soldier's, in voxels: as far as a lean reaches (LeanReach).
const double VrHeadReach = LeanReach;
/// A snap turn turns the world this many degrees (a step of the soldier's facing).
const double VrSnapTurnDegrees = 45.0;
/// The shell's head pose counts for this long after it was last fed (ms); past it the desktop's stand-in looks again.
const Uint32 VrShellHeadTimeout = 1000;
/// The crosshair on the open sky (nothing under it) is drawn this far along the aim (voxels): far enough that both eyes
/// see it in the same place, at the sky's depth.
const double VrSkyAimDistance = 4000.0;
/// Smooth turning (setting "VR turning"): degrees a second with the stick pushed all the way (the owner: "about 90").
const double VrSmoothTurnRate = 90.0;
/// Snap turning with the shell's stick: a snap when it's pushed past this (of all the way), the next once it came back
/// under VrSnapAxisRest.
const double VrSnapAxisPush = 0.6, VrSnapAxisRest = 0.4;
/// The shell's stick and hands count for this long after they were last given (ms); past it the turn keys turn again, and
/// the hands count as not tracked.
const Uint32 VrShellInputTimeout = 1000;
/// Half a sector of the game's eight facings (degrees): the soldier's way may be that far off his facing before he turns.
const double VrSectorHalf = 22.5;
/// The desktop's stand-in right hand: where it holds the weapon from the head's middle (metres: right, up, back).
const double VrStandInHand[3] = { 0.20, -0.32, -0.28 };
/// ...and its left hand, two-handed (the right button held): this far ahead of the right along the head's look (metres).
const double VrStandInLeftReach = 0.45;
/// Two-handed aim needs the hands at least this far apart (metres); nearer, the right hand's own aim.
const double VrTwoHandedLeast = 0.05;
/// The weapon's muzzle stays this far short of anything solid on the way from the eye (voxels), the weapon slid back
/// along its barrel as a real one would be pushed back; looked for every VrRifleCheckStep voxels.
const double VrRifleWallGap = 1.0, VrRifleCheckStep = 0.5;
/// The soldier can't fire outside his arc: said at most this often (ms); the crosshair is then this many shades darker.
const Uint32 VrArcWarnTime = 2000;
const int VrDimShades = 6;

/// Turns a vector by a unit quaternion (x, y, z, w): v + 2w (q x v) + 2 q x (q x v).
void rotateByQuaternion(const float quaternion[4], const double vector[3], double out[3])
{
	const double qx = quaternion[0], qy = quaternion[1], qz = quaternion[2], qw = quaternion[3];
	// t = 2 (q x v), then v + w t + q x t.
	const double tx = 2.0 * (qy * vector[2] - qz * vector[1]);
	const double ty = 2.0 * (qz * vector[0] - qx * vector[2]);
	const double tz = 2.0 * (qx * vector[1] - qy * vector[0]);
	out[0] = vector[0] + qw * tx + (qy * tz - qz * ty);
	out[1] = vector[1] + qw * ty + (qz * tx - qx * tz);
	out[2] = vector[2] + qw * tz + (qx * ty - qy * tx);
}

}

/// The mode VR's calls go to (see vrCurrent).
FirstPersonMode *FirstPersonMode::_vrCurrent = nullptr;

/**
 * Is VR mode on: the setting "VR mode" On, or with the desktop's Preview?
 * @return True if so.
 */
bool FirstPersonMode::vrOn()
{
	return Options::fpsVrMode > 0;
}

/**
 * The battlescape's first-person mode the VR calls go to: the screen made last, while it lives.
 * @return The mode, or null (no battle on).
 */
FirstPersonMode *FirstPersonMode::vrCurrent()
{
	return _vrCurrent;
}

/**
 * Does the 3D view show now, with VR mode on: is there anything for vrRenderEyes to draw?
 * @return True if so.
 */
bool FirstPersonMode::vrStereoReady() const
{
	return vrOn() && isActive() && _trackedUnit;
}

/**
 * Is the player in the 3D view on his turn, with VR mode on (see FirstPerson::vrInThreeD)? He wants
 * it (F8 in, and not out since; aiming on the map is out for its while) and it's the player's
 * turn: the aliens' turn plays on the isometric map in VR (see alienTurn3D). Whether it shows this
 * moment doesn't matter (a screen over it, nobody to look through for a moment).
 * @return True if so.
 */
bool FirstPersonMode::vrInThreeD() const
{
	return vrOn() && _wants3D && _save->getSide() == FACTION_PLAYER;
}

/**
 * The head's pose this frame, from the shell. From now on the desktop's stand-in (the mouse) no
 * longer turns it; the first pose recentres (a headset's yaw is wherever its owner sat down).
 * @param head The pose: the VIEW space in the shell's LOCAL space.
 */
void FirstPersonMode::vrSetHeadPose(const FirstPerson::VrPose &head)
{
	const bool first = !_vrShellHead;
	_vrHead = head;
	_vrShellHead = true;
	_vrShellHeadAt = SDL_GetTicks();
	if (first)
	{
		vrRecentre();
	}
}

/**
 * Recentres: the world turned so that the soldier looked through faces where the head looks now
 * (their turret's facing, a tank with its turret switch on), his way that facing, and the
 * soldier's eye is where the head is now (the desktop's stand-in keeps its own middle).
 */
void FirstPersonMode::vrRecentre()
{
	if (!_trackedUnit)
	{
		return;
	}
	const int facing = turretTurning(_trackedUnit) ? _trackedUnit->getTurretDirection() : _trackedUnit->getDirection();
	vrRecentreOn(facing * 45.0 - 90.0);
}

/**
 * Recentres on a way in the world: the world turned so that where the head looks now (along the
 * ground) looks that way, and the soldier's eye where the head is now (the desktop's stand-in keeps
 * its own middle). The soldier's way (see vrFaceBody) is his facing (his turret's) again.
 * @param worldYawDegrees The way, in degrees: 0 east, 90 south.
 */
void FirstPersonMode::vrRecentreOn(double worldYawDegrees)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		_vrHeadReference[axis] = _vrShellHead ? _vrHead.position[axis] : 0.0f;
	}
	// The head's look in the headset's space, as a turn right from its -z (a head looking almost straight up or down: the
	// way the top of the head points then, reversed looking up).
	const double ahead[3] = { 0.0, 0.0, -1.0 }, above[3] = { 0.0, 1.0, 0.0 };
	double looking[3], headUp[3];
	rotateByQuaternion(_vrHead.orientation, ahead, looking);
	rotateByQuaternion(_vrHead.orientation, above, headUp);
	if (std::hypot(looking[0], looking[2]) < 0.2)
	{
		const double side = looking[1] > 0.0 ? -1.0 : 1.0;
		looking[0] = headUp[0] * side;
		looking[2] = headUp[2] * side;
	}
	const double headYawDegrees = std::atan2(looking[0], -looking[2]) * 180.0 / Pi;
	_vrYawOffset = std::remainder(worldYawDegrees - headYawDegrees, 360.0);
	_vrBodyYaw = worldYawDegrees;
	if (_trackedUnit)
	{
		_vrBodyYaw = (turretTurning(_trackedUnit) ? _trackedUnit->getTurretDirection() : _trackedUnit->getDirection()) * 45.0 - 90.0;
	}
	vrApplyHeadAxes();
}

/**
 * A snap turn (setting "VR turning: Snap": the turn keys, the shell's stick pushed past half-way;
 * a numpad turn whatever the setting): the soldier's way and the world round the player turn 45
 * degrees at once (-1 to the left, 1 to the right), and the soldier's facing after it, a step by
 * the rules (see vrFaceBody). Not while he can't turn now (busy, a shot under way), nor without
 * the time units for the step (with "Turning costs TU"; the game's warning says why).
 * @param direction -1 left, 1 right.
 */
void FirstPersonMode::vrSnapTurn(int direction)
{
	BattleUnit *unit = _trackedUnit;
	if (!vrOn() || direction == 0 || !unit || !canAct() || unit->getStatus() != STATUS_STANDING || !_state->playableUnitSelected()
		|| shotUnderWay())
	{
		return;
	}
	const bool turret = turretTurning(unit);
	const int cost = turret ? 1 : unit->getTurnCost();
	if (Options::fpsTurnCost && !doomModeOn() && (unit->getTimeUnits() < cost || !reserveAllows(unit, cost, 0)))
	{
		_turnRefusedWarned = SDL_GetTicks();
		_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		return;
	}
	const double degrees = direction > 0 ? VrSnapTurnDegrees : -VrSnapTurnDegrees;
	_vrBodyYaw = std::remainder(_vrBodyYaw + degrees, 360.0);
	_vrYawOffset = std::remainder(_vrYawOffset + degrees, 360.0);
	vrApplyHeadAxes();
	vrFaceBody(unit, 0.0);
}

/**
 * The controllers this frame, from the shell (see FirstPerson::vrSetHands): kept for the frame's
 * rules and for the eyes drawn next (see vrUpdateRifle).
 * @param rightAim The right hand's aim pose, or null if not tracked.
 * @param rightGrip Its grip pose, or null.
 * @param leftAim The left hand's aim pose, or null.
 * @param leftGripHeld The left grip held.
 */
void FirstPersonMode::vrSetHands(const FirstPerson::VrPose *rightAim, const FirstPerson::VrPose *rightGrip, const FirstPerson::VrPose *leftAim,
	bool leftGripHeld)
{
	_vrRightAimOk = rightAim != nullptr;
	_vrRightGripOk = rightGrip != nullptr;
	_vrLeftAimOk = leftAim != nullptr;
	if (rightAim) _vrRightAim = *rightAim;
	if (rightGrip) _vrRightGrip = *rightGrip;
	if (leftAim) _vrLeftAim = *leftAim;
	_vrLeftGripHeld = leftGripHeld;
	_vrShellHandsAt = std::max<Uint32>(1, SDL_GetTicks());
	_vrHandsGiven = true;
}

/**
 * The right stick's sideways push this frame, from the shell (see FirstPerson::vrSetTurnAxis):
 * smooth turning turns as far as it's pushed (see vrFaceBody); snap turning snaps once each time
 * it's pushed past VrSnapAxisPush, again once it came back under VrSnapAxisRest.
 * @param axis -1 left .. 1 right.
 */
void FirstPersonMode::vrSetTurnAxis(double axis)
{
	_vrTurnAxis = std::clamp(axis, -1.0, 1.0);
	_vrTurnAxisAt = std::max<Uint32>(1, SDL_GetTicks());
	if (Options::fpsVrTurning == 1)
	{
		if (!_vrTurnAxisSnapped && std::abs(_vrTurnAxis) >= VrSnapAxisPush)
		{
			_vrTurnAxisSnapped = true;
			vrSnapTurn(_vrTurnAxis > 0.0 ? 1 : -1);
		}
		else if (std::abs(_vrTurnAxis) < VrSnapAxisRest)
		{
			_vrTurnAxisSnapped = false;
		}
	}
}

/**
 * The start of a frame (think): with VR mode on, the view's comfort (nothing shakes it), the head
 * moved on (the shell's pose, or with none lately the desktop's stand-in) and the view looking as
 * it does, for everything that reads where the view looks this frame. VR mode switched off: the
 * view's camera goes back to its own yaw and pitch (where the head left them).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::vrBeginFrame(double elapsedSeconds)
{
	if (!vrOn())
	{
		if (_vrWasOn)
		{
			_view->setHeadAxes(nullptr, nullptr, nullptr);
			_view->setComfort(false);
			_vrWasOn = false;
			_vrRifle.held = _vrModelInHand = false;
		}
		return;
	}
	_vrWasOn = true;
	_view->setComfort(true);
	_view->setHeldModel(FirstPersonHeldModel()); // (the weapon in hand handed again when it shows, see updateWeaponInHand)
	if (_vrShellHead && SDL_GetTicks() - _vrShellHeadAt > VrShellHeadTimeout)
	{
		_vrShellHead = false; // (the shell went quiet: the stand-in looks again)
	}
	if (_vrShellHandsAt && SDL_GetTicks() - _vrShellHandsAt > VrShellInputTimeout)
	{
		_vrShellHandsAt = 0; // (no hands from the shell lately: none tracked)
		_vrRightAimOk = _vrRightGripOk = _vrLeftAimOk = _vrLeftGripHeld = false;
	}
	if (!_vrShellHead)
	{
		vrStandIn(elapsedSeconds);
	}
	vrApplyHeadAxes();
}

/**
 * The desktop's stand-in head and hands this frame. The head: turned as the look keys turned it
 * (see turnWithKeys), and leaning with the lean keys as the eye leans on the desktop (setting
 * "Lean"; see updateLean): into the lean over LeanTime, eased in and out, the head going LeanReach
 * to the side of where it looks and LeanDrop lower, tilted LeanTiltDegrees with "Tilt". It stands
 * where the last recentring left the soldier's eye (its middle, 0). The hands (the preview's,
 * while the shell gives none): the right one at VrStandInHand, aimed as the mouse turned it (see
 * turnByMouse); with the right button held, the left one two-handed, VrStandInLeftReach ahead of
 * the right along the head's look (the barrel then points where the head looks).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::vrStandIn(double elapsedSeconds)
{
	// Which way the lean keys lean: -1 left, 1 right, 0 for neither or both (or leaning off).
	auto held = [](SDL_Keycode key) { return isKeyHeld(key); };
	double wanted = 0.0;
	if (Options::fpsLean > 0 && _trackedUnit && !_trackedUnit->isOut())
	{
		const bool leanKey = held(Options::keyFpsLeanHold);
		const bool right = held(Options::keyFpsLeanRightOwn) || (leanKey && held(Options::keyFpsStrafeRight));
		const bool left = held(Options::keyFpsLeanLeftOwn) || (leanKey && held(Options::keyFpsStrafeLeft));
		wanted = (right ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
	}
	_lean += std::clamp(wanted - _lean, -elapsedSeconds / LeanTime, elapsedSeconds / LeanTime);
	const double progress = std::abs(_lean);
	const double amount = std::copysign(progress * progress * (3.0 - 2.0 * progress), _lean); // -1 .. 1

	// The head's way of looking: the mouse's turn and look, the lean's tilt (positive tilts it left).
	FirstPerson::vrHeadOrientation(_vrStandInYaw, _vrStandInPitch, Options::fpsLean >= 2 ? -amount * LeanTiltDegrees : 0.0, _vrHead.orientation);
	// Its place: to the side of where it looks along the ground (its level right, (cos, 0, sin) of the turn), and lower.
	const double yawRadians = _vrStandInYaw * Pi / 180.0;
	const double sideways = amount * LeanReach * FirstPerson::VrMetresPerVoxel;
	const double lower = std::abs(amount) * LeanDrop * VrVoxelHeight * FirstPerson::VrMetresPerVoxel;
	_vrHead.position[0] = (float)(std::cos(yawRadians) * sideways);
	_vrHead.position[1] = (float)-lower;
	_vrHead.position[2] = (float)(std::sin(yawRadians) * sideways);

	// The hands (the desktop's preview only: VR mode On on the desktop draws the head's own view, aiming with the head).
	if (!_vrShellHandsAt && Options::fpsVrMode == 2)
	{
		FirstPerson::VrPose hand;
		for (int axis = 0; axis < 3; ++axis)
		{
			hand.position[axis] = (float)VrStandInHand[axis];
		}
		FirstPerson::vrHeadOrientation(_vrStandInAimYaw, _vrStandInAimPitch, 0.0, hand.orientation);
		_vrRightAim = _vrRightGrip = hand;
		_vrRightAimOk = _vrRightGripOk = true;
		_vrLeftGripHeld = _vrLeftAimOk = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
		if (_vrLeftAimOk)
		{
			const double ahead[3] = { 0.0, 0.0, -1.0 };
			double looking[3];
			rotateByQuaternion(_vrHead.orientation, ahead, looking);
			for (int axis = 0; axis < 3; ++axis)
			{
				_vrLeftAim.position[axis] = (float)(hand.position[axis] + looking[axis] * VrStandInLeftReach);
			}
			std::copy(_vrHead.orientation, _vrHead.orientation + 4, _vrLeftAim.orientation);
		}
	}
	else if (!_vrShellHandsAt)
	{
		_vrRightAimOk = _vrRightGripOk = _vrLeftAimOk = _vrLeftGripHeld = false;
	}
}

/**
 * The weapon in the right hand this frame (see _vrRifle), from the hands' poses: none without a
 * right hand tracked (then the weapon is where the flat view has it, aiming with the head). Its
 * barrel points the way the right hand's aim points (its -z), or with the left grip held from the
 * right hand through the left; its up is the right hand's, made square to that. The hand (its grip
 * pose, else its aim pose) holds it, placed from the head as the eyes are (see vrEyeCamera); its
 * muzzle is _vrMuzzleAhead further along the barrel (as the weapon's model last had it, see
 * updateWeaponInHand). Near a wall the weapon is slid back along the line from the eye to its muzzle
 * (the hand with it, as a real one would be pushed back), so the muzzle stays VrRifleWallGap short
 * of anything solid on the way (looked for every VrRifleCheckStep voxels, as the game's voxels
 * have it, teammates too): the laser, the aim and our shots start on the player's side of it.
 * @param unit The soldier looked through.
 */
void FirstPersonMode::vrUpdateRifle(BattleUnit *unit)
{
	_vrRifle.held = false;
	if (!vrOn() || !unit || (!_vrRightAimOk && !_vrRightGripOk))
	{
		return;
	}
	const FirstPerson::VrPose &aimPose = _vrRightAimOk ? _vrRightAim : _vrRightGrip;
	const FirstPerson::VrPose &gripPose = _vrRightGripOk ? _vrRightGrip : _vrRightAim;
	VrRifle rifle;
	vrAxes(aimPose.orientation, rifle.ahead, rifle.right, rifle.up);

	// Two-handed: the barrel from the right hand through the left; its up square to that, its right square to both.
	if (_vrLeftGripHeld && _vrLeftAimOk)
	{
		const double between[3] = { (double)_vrLeftAim.position[0] - gripPose.position[0], (double)_vrLeftAim.position[1] - gripPose.position[1],
			(double)_vrLeftAim.position[2] - gripPose.position[2] };
		const double apart = std::sqrt(between[0] * between[0] + between[1] * between[1] + between[2] * between[2]);
		if (apart > VrTwoHandedLeast)
		{
			vrToWorld(between, rifle.ahead);
			for (double &component : rifle.ahead) component /= apart;
			const double upAlong = rifle.up[0] * rifle.ahead[0] + rifle.up[1] * rifle.ahead[1] + rifle.up[2] * rifle.ahead[2];
			double upLength = 0.0;
			for (int axis = 0; axis < 3; ++axis)
			{
				rifle.up[axis] -= rifle.ahead[axis] * upAlong;
				upLength += rifle.up[axis] * rifle.up[axis];
			}
			upLength = std::sqrt(upLength);
			if (upLength < 1e-6)
			{
				// (pointing straight up or down: the head's up stands in)
				double headAhead[3], headRight[3];
				vrAxes(_vrHead.orientation, headAhead, headRight, rifle.up);
				upLength = 1.0;
			}
			for (double &component : rifle.up) component /= upLength;
			rifle.right[0] = rifle.up[1] * rifle.ahead[2] - rifle.up[2] * rifle.ahead[1];
			rifle.right[1] = rifle.up[2] * rifle.ahead[0] - rifle.up[0] * rifle.ahead[2];
			rifle.right[2] = rifle.up[0] * rifle.ahead[1] - rifle.up[1] * rifle.ahead[0];
		}
	}

	// Where the hand holds it, and its muzzle, in voxels.
	const double fromHeadInTracking[3] = { (double)gripPose.position[0] - _vrHead.position[0], (double)gripPose.position[1] - _vrHead.position[1],
		(double)gripPose.position[2] - _vrHead.position[2] };
	double fromHead[3];
	vrToWorld(fromHeadInTracking, fromHead);
	double eye[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		const double voxelsUp = axis == 2 ? VrVoxelHeight : 1.0;
		eye[axis] = _vrSoldierEye[axis] + _vrHeadOffset[axis];
		rifle.grip[axis] = eye[axis] + fromHead[axis] / FirstPerson::VrMetresPerVoxel / voxelsUp;
		rifle.muzzle[axis] = rifle.grip[axis] + rifle.ahead[axis] * _vrMuzzleAhead / voxelsUp;
	}

	// Slid back from anything solid on the way from the eye to the muzzle.
	const double toMuzzle[3] = { rifle.muzzle[0] - eye[0], rifle.muzzle[1] - eye[1], rifle.muzzle[2] - eye[2] };
	const double reach = std::sqrt(toMuzzle[0] * toMuzzle[0] + toMuzzle[1] * toMuzzle[1] + toMuzzle[2] * toMuzzle[2]);
	if (reach > 1e-6)
	{
		TileEngine *tileEngine = _save->getTileEngine();
		double clear = reach;
		for (double distance = VrRifleCheckStep; distance <= reach + VrRifleWallGap; distance += VrRifleCheckStep)
		{
			const Position voxel((int)std::floor(eye[0] + toMuzzle[0] / reach * distance), (int)std::floor(eye[1] + toMuzzle[1] / reach * distance),
				(int)std::floor(eye[2] + toMuzzle[2] / reach * distance));
			if (tileEngine->voxelCheck(voxel, unit, false, true) != V_EMPTY)
			{
				clear = std::max(0.0, distance - VrRifleCheckStep - VrRifleWallGap);
				break;
			}
		}
		if (clear < reach)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				const double back = toMuzzle[axis] / reach * (reach - clear);
				rifle.muzzle[axis] -= back;
				rifle.grip[axis] -= back;
			}
		}
	}
	// The barrel's way in the voxel grid, for the aim's line.
	double gridLength = 0.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		rifle.aimGrid[axis] = rifle.ahead[axis] / (axis == 2 ? VrVoxelHeight : 1.0);
		gridLength += rifle.aimGrid[axis] * rifle.aimGrid[axis];
	}
	gridLength = std::sqrt(gridLength);
	for (double &component : rifle.aimGrid) component /= gridLength;
	rifle.held = true;
	_vrRifle = rifle;
}

/**
 * A ray from the weapon's muzzle (see vrUpdateRifle), off its barrel by so much to its right and
 * up (a round's spread, see freeAimTarget).
 * @param tangentRight The tangent of the angle off the barrel to its right.
 * @param tangentUp ...and up.
 * @param origin Set to the muzzle, in voxels.
 * @param direction Set to the way, a unit vector in the voxel grid.
 */
void FirstPersonMode::vrRifleRay(double tangentRight, double tangentUp, double origin[3], double direction[3]) const
{
	double length = 0.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		origin[axis] = _vrRifle.muzzle[axis];
		direction[axis] = (_vrRifle.ahead[axis] + tangentRight * _vrRifle.right[axis] + tangentUp * _vrRifle.up[axis]) / (axis == 2 ? VrVoxelHeight : 1.0);
		length += direction[axis] * direction[axis];
	}
	length = std::sqrt(length);
	for (int axis = 0; axis < 3; ++axis)
	{
		direction[axis] /= length;
	}
}

/**
 * The soldier's way this frame, turned by the right stick (setting "VR turning: Smooth": the
 * shell's stick, while it gives one, else the turn keys held; snap turning turns on a push, see
 * vrSnapTurn), VrSmoothTurnRate degrees a second at full push, and his facing after it by the
 * rules (see vrTurnBody). The head and the weapon never turn him.
 * @param unit The soldier looked through.
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::vrFaceBody(BattleUnit *unit, double elapsedSeconds)
{
	double push = 0.0;
	if (Options::fpsVrTurning == 0 && elapsedSeconds > 0.0)
	{
		if (vrTurnAxisFresh())
		{
			push = _vrTurnAxis;
		}
		else if (!gameModifierHeld())
		{
			push = (isKeyHeld(Options::keyFpsTurnRight) ? 1.0 : 0.0) - (isKeyHeld(Options::keyFpsTurnLeft) ? 1.0 : 0.0);
		}
	}
	vrTurnBody(unit, push * VrSmoothTurnRate * elapsedSeconds);
}

/**
 * A turn key pressed in VR mode (the desktop's right stick): a snap turn with "VR turning: Snap"
 * (see vrSnapTurn); smooth turning turns while it's held (see vrFaceBody). Nothing while the
 * shell's stick turns (it may press the keys too).
 * @param direction -1 left, 1 right.
 */
void FirstPersonMode::vrTurnKey(int direction)
{
	if (Options::fpsVrTurning == 1 && !vrTurnAxisFresh())
	{
		vrSnapTurn(direction);
	}
}

/**
 * Does the shell's stick turn now: has it been given lately (see vrSetTurnAxis)? The turn keys
 * then don't turn.
 * @return True if so.
 */
bool FirstPersonMode::vrTurnAxisFresh() const
{
	return _vrTurnAxisAt && SDL_GetTicks() - _vrTurnAxisAt <= VrShellInputTimeout;
}

/**
 * Turns the soldier's way (and the world round the player with it) by so many degrees as the
 * rules let it go this frame, and his facing (his turret's, a tank with its turret switch on)
 * after it: the game's eight facings change as his way crosses each sector (VrSectorHalf off his
 * facing). With "Turning costs TU" a step through the game's own turning, the armor's turn cost
 * (a turret's 1), his way running at most a sector ahead meanwhile; without the time units (or
 * reserved for a shot) he stops turning, his way held at the sector's edge, and the game's warning
 * says why (at most every two seconds, while the stick turns). With free turning he faces his
 * way's sector at once, for free. While he can't turn (busy, a shot under way) his way stays in
 * his facing's sector. A facing the game changed (a step backwards turns him round to walk it) is
 * turned back to his way, a step at a time, by the same rules; his way is never pulled after it.
 * @param unit The soldier looked through.
 * @param degrees How far to turn his way: positive to the right.
 */
void FirstPersonMode::vrTurnBody(BattleUnit *unit, double degrees)
{
	if (!unit)
	{
		return;
	}
	const bool turret = turretTurning(unit);
	const int facing = turret ? unit->getTurretDirection() : unit->getDirection();
	const double facingYaw = facing * 45.0 - 90.0;
	const double offBefore = std::remainder(_vrBodyYaw - facingYaw, 360.0);
	double off = offBefore + degrees;
	const bool mayTurn = canAct() && unit->getStatus() == STATUS_STANDING && _state->playableUnitSelected() && !shotUnderWay()
		&& unit->getFaction() == FACTION_PLAYER && !unit->isOut();
	double limit = 180.0; // how far off his facing his way may go this frame
	if (std::abs(off) > VrSectorHalf)
	{
		if (!mayTurn)
		{
			limit = unit->getStatus() == STATUS_TURNING ? VrSectorHalf + 45.0 : VrSectorHalf;
		}
		else if (!Options::fpsTurnCost)
		{
			faceFreely(unit, (((int)std::lround((facingYaw + off + 90.0) / 45.0)) % 8 + 8) % 8, turret);
		}
		else
		{
			const int step = off > 0.0 ? 1 : -1;
			const int cost = turret ? 1 : unit->getTurnCost();
			if (!doomModeOn() && (unit->getTimeUnits() < cost || !reserveAllows(unit, cost, 0)))
			{
				limit = VrSectorHalf;
				const Uint32 now = SDL_GetTicks();
				if (degrees != 0.0 && now - _turnRefusedWarned > 2000)
				{
					_turnRefusedWarned = now;
					_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
				}
			}
			else
			{
				Position towards;
				Pathfinding::directionToVector((facing + step + 8) % 8, &towards);
				BattleAction turn;
				turn.actor = unit;
				turn.target = unit->getPosition() + towards;
				turn.strafe = turret;
				_state->_battleGame->statePushBack(new UnitTurnBState(_state->_battleGame, turn));
				limit = VrSectorHalf + 45.0;
			}
		}
	}
	if (std::abs(off) > limit && std::abs(off) > std::abs(offBefore))
	{
		off = std::copysign(std::max(limit, std::abs(offBefore)), off);
	}
	const double turned = off - offBefore;
	if (turned != 0.0)
	{
		_vrBodyYaw = std::remainder(_vrBodyYaw + turned, 360.0);
		_vrYawOffset = std::remainder(_vrYawOffset + turned, 360.0);
		vrApplyHeadAxes();
	}
}

/**
 * The game's hook for a shot or a throw being fired (see FirstPerson::shotKeepsFacing): in VR mode,
 * by the soldier the 3D view looks through, it's fired without the game's turn to the target: the
 * view fires only within his front arc (see fire, vrInArc), and nothing turns him but the right
 * stick (the owner: "no automatic turning for a shot").
 * @param state The battlescape screen.
 * @param action The shot or throw.
 * @return True to fire it without turning.
 */
bool FirstPersonMode::shotKeepsFacing(BattlescapeState *state, const BattleAction &action)
{
	return vrOn() && state && state->_firstPerson && state->_firstPerson->isActive() && action.actor
		&& action.actor == state->_firstPerson->_trackedUnit;
}

/**
 * Is a tile within the soldier's front arc: what he sees by the game's rules (BattleUnit::
 * checkViewSector: the quarter in front of his facing, his turret's for a tank as the game has
 * it)? A shot there needs no turn; VR mode turns him for none (the owner: "no automatic turning for
 * a shot").
 * @param unit The soldier.
 * @param tile The tile.
 * @return True if so.
 */
bool FirstPersonMode::vrInArc(const BattleUnit *unit, const Position &tile) const
{
	return unit && unit->checkViewSector(tile, Options::strafe && unit->getTurretType() > -1);
}

/**
 * Says, at most every VrArcWarnTime, that the soldier can't fire outside his arc (see vrInArc).
 */
void FirstPersonMode::vrWarnArc()
{
	const Uint32 now = SDL_GetTicks();
	if (now - _vrArcWarned > VrArcWarnTime)
	{
		_vrArcWarned = now;
		_state->warning("STR_FPS_VR_OUT_OF_ARC");
	}
}

/**
 * The way the movement keys walk (and the numpad steps): in VR the soldier's way, which only the
 * right stick turns (the owner: "the left stick walks and strafes relative to his facing"); else
 * where the view looks.
 * @return The yaw, degrees: 0 east, 90 south.
 */
double FirstPersonMode::moveYawDegrees() const
{
	return vrOn() ? _vrBodyYaw : _view->getYawDegrees();
}

/**
 * A direction in the headset's space as the world has it: the headset's forward (-z) along the
 * world's yaw the last recentring set, its right (x) to the right of that along the ground, its up
 * (y) the world's up. In true proportions (see the 3D view's ZScale): x east, y south, z up.
 * @param tracking The direction (x right, y up, z back), any length.
 * @param world Set to it in the world, as long.
 */
void FirstPersonMode::vrToWorld(const double tracking[3], double world[3]) const
{
	const double yaw = _vrYawOffset * Pi / 180.0;
	const double forwardX = std::cos(yaw), forwardY = std::sin(yaw), rightX = -std::sin(yaw), rightY = std::cos(yaw);
	world[0] = -tracking[2] * forwardX + tracking[0] * rightX;
	world[1] = -tracking[2] * forwardY + tracking[0] * rightY;
	world[2] = tracking[1];
}

/**
 * A pose's axes in the world (true proportions): where it looks (its -z), its right (x) and its up
 * (y), turned by its orientation, then into the world (see vrToWorld).
 * @param orientation The pose's orientation, a unit quaternion (x, y, z, w).
 * @param forward Set to where it looks.
 * @param right Set to its right.
 * @param up Set to its up.
 */
void FirstPersonMode::vrAxes(const float orientation[4], double forward[3], double right[3], double up[3]) const
{
	const double ahead[3] = { 0.0, 0.0, -1.0 }, toRight[3] = { 1.0, 0.0, 0.0 }, above[3] = { 0.0, 1.0, 0.0 };
	double turned[3];
	rotateByQuaternion(orientation, ahead, turned);
	vrToWorld(turned, forward);
	rotateByQuaternion(orientation, toRight, turned);
	vrToWorld(turned, right);
	rotateByQuaternion(orientation, above, turned);
	vrToWorld(turned, up);
}

/**
 * The view looks the way the head does: its axes in the world (see vrAxes, FirstPersonView::
 * setHeadAxes), with no limit and no recoil. The eye isn't moved (see vrPlaceEye).
 */
void FirstPersonMode::vrApplyHeadAxes()
{
	double forward[3], right[3], up[3];
	vrAxes(_vrHead.orientation, forward, right, up);
	_view->setHeadAxes(forward, right, up);
}

/**
 * Places the eye in VR mode (the end of updateCamera): the soldier's own eye (where the camera
 * stands, walking, riding or falling, at eye height, a jump on top, kept out of anything solid as
 * on the desktop: see clearEye), and nothing else moving it but the head (see vrMoveHead). That
 * move along the ground is the lean the game's shots know (see leanSide, leaningOut,
 * freeAimOrigin): to the side of where the view looks, past LeanThreshold, a shot leaves from that
 * side.
 * @param unit The soldier looked through.
 */
void FirstPersonMode::vrPlaceEye(BattleUnit *unit)
{
	// The soldier's own eye.
	double eye[3] = { _eyeX, _eyeY, _eyeZ + currentJumpHeight(unit) };
	clearEye(unit, eye, _view->getYawDegrees() * Pi / 180.0);
	std::copy(eye, eye + 3, _vrSoldierEye);

	// The head's move, and the lean the game's shots know: the move along the ground, and how far of it is to the side
	// of the look.
	vrMoveHead(unit);
	const double yawRadians = _view->getYawDegrees() * Pi / 180.0;
	_leanShift[0] = _vrHeadOffset[0];
	_leanShift[1] = _vrHeadOffset[1];
	_leanOffset = _vrHeadOffset[0] * -std::sin(yawRadians) + _vrHeadOffset[1] * std::cos(yawRadians);
	vrApplyHeadAxes();
	vrUpdateRifle(unit);
}

/**
 * Moves the eye with the head, from the soldier's eye (see vrPlaceEye): by the head's move since
 * the recentring (metres into voxels; a voxel up is VrVoxelHeight of one across), at most
 * VrHeadReach, stopped LeanWallMargin short of anything solid on its way (as a lean stops: see
 * leanRoom), so the eye never looks into a wall or a teammate's head. Again when the eyes are
 * drawn (see vrRenderEyes), for the shell's newer pose.
 * @param unit The soldier looked through.
 */
void FirstPersonMode::vrMoveHead(BattleUnit *unit)
{
	// The head's move since the recentring, in the world, in voxels across (true proportions), at most VrHeadReach.
	const double movedInTracking[3] = { (double)_vrHead.position[0] - _vrHeadReference[0], (double)_vrHead.position[1] - _vrHeadReference[1],
		(double)_vrHead.position[2] - _vrHeadReference[2] };
	double moved[3];
	vrToWorld(movedInTracking, moved);
	double length = 0.0;
	for (double &component : moved)
	{
		component /= FirstPerson::VrMetresPerVoxel;
		length += component * component;
	}
	length = std::sqrt(length);
	double reach = std::min(length, VrHeadReach);
	// ...stopped short of anything solid on its way (looked for every LeanCheckStep voxels, as a lean's room is).
	TileEngine *tileEngine = _save->getTileEngine();
	const double *eye = _vrSoldierEye;
	if (length > 1e-6)
	{
		for (double distance = LeanCheckStep; distance <= reach + LeanWallMargin; distance += LeanCheckStep)
		{
			const Position voxel((int)std::floor(eye[0] + moved[0] / length * distance), (int)std::floor(eye[1] + moved[1] / length * distance),
				(int)std::floor(eye[2] + moved[2] / length * distance / VrVoxelHeight));
			if (tileEngine->voxelCheck(voxel, unit, false, true) != V_EMPTY)
			{
				reach = std::min(reach, std::max(0.0, distance - LeanCheckStep - LeanWallMargin));
				break;
			}
		}
	}
	const double share = length > 1e-6 ? reach / length : 0.0;
	_vrHeadOffset[0] = moved[0] * share;
	_vrHeadOffset[1] = moved[1] * share;
	_vrHeadOffset[2] = moved[2] * share / VrVoxelHeight; // (into voxels up)

	// The view: the eye there.
	_view->setEye(eye[0] + _vrHeadOffset[0], eye[1] + _vrHeadOffset[1], eye[2] + _vrHeadOffset[2]);
}

/**
 * An eye of the shell's (or the stand-in's) as the 3D view's camera for it: its axes (see vrAxes),
 * its place (the head's eye, see vrPlaceEye, and the eye's own offset from the head, turned into
 * the world), and its field of view's four tangents.
 * @param view The eye: its pose (in the head's space) and field of view.
 * @param camera Set to the camera.
 */
void FirstPersonMode::vrEyeCamera(const FirstPerson::VrEyeView &view, FirstPersonEyeCamera &camera) const
{
	vrAxes(view.pose.orientation, camera.forward, camera.right, camera.up);
	const double fromHeadInTracking[3] = { (double)view.pose.position[0] - _vrHead.position[0], (double)view.pose.position[1] - _vrHead.position[1],
		(double)view.pose.position[2] - _vrHead.position[2] };
	double fromHead[3];
	vrToWorld(fromHeadInTracking, fromHead);
	for (int axis = 0; axis < 3; ++axis)
	{
		const double inVoxels = fromHead[axis] / FirstPerson::VrMetresPerVoxel / (axis == 2 ? VrVoxelHeight : 1.0);
		camera.eye[axis] = _vrSoldierEye[axis] + _vrHeadOffset[axis] + inVoxels;
	}
	camera.tangentLeft = std::tan(view.fov.angleLeft);
	camera.tangentRight = std::tan(view.fov.angleRight);
	camera.tangentUp = std::tan(view.fov.angleUp);
	camera.tangentDown = std::tan(view.fov.angleDown);
}

/**
 * The desktop stand-in's two eyes: both look as its head looks (a headset's eyes are parallel),
 * VrStandInEyeDistance apart along its right, each seeing as a Quest 3's eye does
 * (VrStandInFov: further out than in, further down than up).
 * @param views Set to the left eye's view and the right eye's.
 */
void FirstPersonMode::vrStandInViews(FirstPerson::VrEyeView views[2]) const
{
	const double toRight[3] = { 1.0, 0.0, 0.0 };
	double right[3];
	rotateByQuaternion(_vrHead.orientation, toRight, right);
	for (int eye = 0; eye < 2; ++eye)
	{
		const double side = (eye == 0 ? -0.5 : 0.5) * VrStandInEyeDistance;
		for (int axis = 0; axis < 3; ++axis)
		{
			views[eye].pose.position[axis] = (float)(_vrHead.position[axis] + right[axis] * side);
		}
		std::copy(_vrHead.orientation, _vrHead.orientation + 4, views[eye].pose.orientation);
		views[eye].fov.angleLeft = (float)(-VrStandInFov[eye][0] * Pi / 180.0);
		views[eye].fov.angleRight = (float)(VrStandInFov[eye][1] * Pi / 180.0);
		views[eye].fov.angleUp = (float)(VrStandInFov[eye][2] * Pi / 180.0);
		views[eye].fov.angleDown = (float)(-VrStandInFov[eye][3] * Pi / 180.0);
	}
}

/**
 * Draws both eyes for these views (see FirstPerson::vrRenderEyes): the view looking as the head
 * does, each eye's camera, the point the crosshair is on (what the aim found under it, else far
 * along the head's look: the open sky), the 3D view's two pictures (FirstPersonView::renderEyes),
 * and the crosshair on each at that point's place, so it shows at the depth of what it points at
 * (as big for the eye as on the desktop's view; not while the crosshair is hidden). The weapon in
 * hand is drawn in each (see updateWeaponInHand), its space the head's (see vrHeldFrame). The head
 * is the shell's newest (set after the frame, see its hook): the eye is moved with it and the
 * crosshair's point found again for it, so neither lags a frame behind the head. Each picture
 * comes with its depth (the view's depth buffer for that eye, in voxels across).
 * @param views The left eye's and the right eye's.
 * @param width Each picture's width, in pixels.
 * @param height Its height.
 * @param pictures Set to the two pictures.
 * @param hand The hook for a controller's hand: the weapon's space at this pose (null: the head's).
 * @return False with nothing to draw (the pictures left empty).
 */
bool FirstPersonMode::vrRenderEyes(const FirstPerson::VrEyeView views[2], int width, int height, FirstPerson::VrPicture pictures[2], const FirstPerson::VrPose *hand,
	int cardSlot, bool cardCheck)
{
	pictures[0] = pictures[1] = FirstPerson::VrPicture();
	if (!vrStereoReady() || width < 16 || height < 16)
	{
		return false;
	}
	// The head and hands as the shell has them now (its hook sets them after the frame: newer than the ones the frame
	// placed the eye and aimed with; the earlier hook's hand pose stands in for a right hand the shell gave no hands
	// for): looking that way, the eye moved with it, the weapon in the right hand, each eye's camera.
	if (hand && !_vrHandsGiven)
	{
		_vrRightAim = _vrRightGrip = *hand;
		_vrRightAimOk = _vrRightGripOk = true;
		_vrLeftAimOk = _vrLeftGripHeld = false;
	}
	_vrHandsGiven = false;
	vrApplyHeadAxes();
	vrMoveHead(_trackedUnit);
	vrUpdateRifle(_trackedUnit);
	FirstPersonEyeCamera cameras[2];
	vrEyeCamera(views[0], cameras[0]);
	vrEyeCamera(views[1], cameras[1]);

	// The point the crosshair is on: where the weapon points (with no right hand what the head looks at; see aimAt), on
	// the open sky far along that line; dimmed outside the soldier's arc, where he can't fire (see vrInArc). The weapon's
	// laser from its muzzle to there (setting "VR laser").
	double aim[3], aimFrom[3], aimWay[3];
	Position aimVoxel;
	const bool playerTurn = _save->getSide() == FACTION_PLAYER;
	aimRay(_view->getWidth() / 2, _view->getHeight() / 2, aimFrom, aimWay);
	const bool aimHits = aimAt(_view->getWidth() / 2, _view->getHeight() / 2, _trackedUnit, aimVoxel, nullptr);
	const int voxelAt[3] = { aimVoxel.x, aimVoxel.y, aimVoxel.z };
	for (int axis = 0; axis < 3; ++axis)
	{
		aim[axis] = playerTurn && aimHits ? voxelAt[axis] + 0.5 : aimFrom[axis] + aimWay[axis] * VrSkyAimDistance;
	}
	const bool dimmed = playerTurn && !vrInArc(_trackedUnit, aimedTile(aimVoxel));
	if (Options::fpsVrLaser && vrRifleAims() && playerTurn)
	{
		_view->setLaser(_vrRifle.muzzle, aim, dimmed);
	}
	FirstPersonHeldFrame handFrame;
	vrHeldFrame(views, hand, handFrame);
	// Each picture gets the crosshair (its surface's pixels, every one of them a square of `scale` pixels: as many as
	// the eye's pixels a degree are times the desktop view's) where the aim point is on it; dimmed, each colour a few
	// shades darker in its group of sixteen.
	const double desktopFocal = (_view->getWidth() / 2.0) / std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0);
	SDL_Surface *crosshair = _crosshair->getSurface();
	auto drawCrosshair = [&](int eye)
	{
		const FirstPersonView::EyePicture picture = _view->eyePicture(eye);
		if (picture.aimShown && _crosshair->getVisible())
		{
			const double eyeFocal = picture.width / std::max(1e-6, picture.tangentRight - picture.tangentLeft);
			const int scale = std::max(1, (int)std::lround(eyeFocal / desktopFocal));
			const int left = (int)std::floor(picture.aimX) - (crosshair->w / 2) * scale, top = (int)std::floor(picture.aimY) - (crosshair->h / 2) * scale;
			for (int crossY = 0; crossY < crosshair->h; ++crossY)
			{
				const Uint8 *crossRow = (const Uint8 *)crosshair->pixels + crossY * crosshair->pitch;
				for (int crossX = 0; crossX < crosshair->w; ++crossX)
				{
					if (!crossRow[crossX]) continue;
					for (int y = top + crossY * scale; y < top + (crossY + 1) * scale; ++y)
					{
						if (y < 0 || y >= picture.height) continue;
						for (int x = left + crossX * scale; x < left + (crossX + 1) * scale; ++x)
						{
							if (x < 0 || x >= picture.width) continue;
							const Uint8 color = dimmed ? (Uint8)((crossRow[crossX] & 0xF0) | std::min(15, (crossRow[crossX] & 15) + VrDimShades)) : crossRow[crossX];
							if (picture.pixels) picture.pixels[y * picture.pitch + x] = color;
							// (An eye kept on the graphics card: over everything on the picture the card lays over its own.)
							if (picture.cardPixels) picture.cardPixels[y * picture.pitch + x] = color;
							if (picture.cardTest) picture.cardTest[y * picture.width + x] = 0.0f;
						}
					}
				}
			}
		}
	};
	// Kept on the graphics card, if the caller has a display that takes them from there (setting "Renderer": GPU): then
	// the view calls for each eye's crosshair before the card finishes that eye; else it goes on the pictures after.
	FirstPersonView::CardRequest card;
	card.mode = cardSlot < 0 || Options::fpsRenderer != 1 ? FirstPersonView::EyesOnCard::No : cardCheck ? FirstPersonView::EyesOnCard::Check : FirstPersonView::EyesOnCard::Yes;
	card.slot = std::max(0, cardSlot);
	card.depthToMetres = (float)FirstPerson::VrMetresPerVoxel;
	card.nearMetres = FirstPerson::VrDepthNearMetres;
	card.farMetres = FirstPerson::VrDepthFarMetres;
	bool crosshairDrawn[2] = { false, false };
	card.drawnLast = [&](int eye) { drawCrosshair(eye); crosshairDrawn[eye] = true; };
	_view->renderEyes(cameras, width, height, aim, &handFrame, card.mode != FirstPersonView::EyesOnCard::No ? &card : nullptr);
	for (int eye = 0; eye < 2; ++eye)
	{
		if (!crosshairDrawn[eye]) drawCrosshair(eye);
		const FirstPersonView::EyePicture picture = _view->eyePicture(eye);
		pictures[eye].pixels = picture.pixels;
		pictures[eye].width = picture.width;
		pictures[eye].height = picture.height;
		pictures[eye].pitch = picture.pitch;
		pictures[eye].palette = _view->getPalette();
		pictures[eye].fov.angleLeft = (float)std::atan(picture.tangentLeft);
		pictures[eye].fov.angleRight = (float)std::atan(picture.tangentRight);
		pictures[eye].fov.angleUp = (float)std::atan(picture.tangentUp);
		pictures[eye].fov.angleDown = (float)std::atan(picture.tangentDown);
		pictures[eye].depth = picture.depth;
		pictures[eye].depthPitch = picture.depth ? picture.width : 0;
		pictures[eye].depthToMetres = (float)FirstPerson::VrMetresPerVoxel;
	}
	// The eyes the card kept: its textures and their fence. An eye it was to keep and couldn't has no picture: no pair
	// this frame (the next comes back the usual way: the view then keeps none on the card).
	FirstPersonView::CardEyes kept;
	if (_view->cardEyes(kept))
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			pictures[eye].colorTexture = kept.colorTextures[eye];
			pictures[eye].depthTexture = kept.depthTextures[eye];
			pictures[eye].fence = kept.fence;
		}
	}
	else if (!pictures[0].pixels || !pictures[1].pixels)
	{
		return false;
	}
	return true;
}

/**
 * Does VR draw both eyes now: the desktop's preview (the setting's third value), or the shell
 * feeding the head and drawing them? Then the weapon in hand goes into the eyes as a 3D object
 * (setting "VR weapon"), not onto the screen or the HUD.
 * @return True if so.
 */
bool FirstPersonMode::vrEyesDrawn() const
{
	return vrOn() && (Options::fpsVrMode == 2 || _vrShellHead);
}

/**
 * Where the hand's space is in the world, for VR's weapon in hand this frame (x right, y down,
 * z ahead, in the desktop weapon's units: see FirstPersonView::setHeldModel). By default the
 * head's, as the eyes drawn now have it (their middle, and their orientation: a headset's eyes
 * look the same way), so the weapon is where the desktop's "Weapon in hand" puts it relative to
 * the head (at the desktop's default field of view its lens shows it at the very angles it has
 * on the desktop, see WeaponLens). The hook for a later version: a hand's pose (a controller's
 * grip, in the views' space) puts it there instead, placed from the head as the eyes are (see
 * vrEyeCamera).
 * @param views The eyes drawn now: the left, the right.
 * @param hand The hand's pose, or null for the head's.
 * @param frame Set to the frame.
 */
void FirstPersonMode::vrHeldFrame(const FirstPerson::VrEyeView views[2], const FirstPerson::VrPose *hand, FirstPersonHeldFrame &frame) const
{
	// The weapon in the right hand (its model made for the hand): its grip at the origin, its barrel ahead, slid back from
	// walls already (see vrUpdateRifle).
	if (_vrModelInHand)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			frame.origin[axis] = _vrRifle.grip[axis];
			frame.ahead[axis] = _vrRifle.ahead[axis];
			frame.right[axis] = _vrRifle.right[axis];
			frame.down[axis] = -_vrRifle.up[axis];
		}
		frame.pullIn = false;
		return;
	}
	frame.pullIn = true;
	// The pose: the hand's, or the eyes' middle and their orientations' average (the second turned to the first's side).
	FirstPerson::VrPose pose;
	if (hand)
	{
		pose = *hand;
	}
	else
	{
		const float *first = views[0].pose.orientation, *second = views[1].pose.orientation;
		const float sign = first[0] * second[0] + first[1] * second[1] + first[2] * second[2] + first[3] * second[3] < 0.0f ? -1.0f : 1.0f;
		double length = 0.0;
		for (int index = 0; index < 4; ++index)
		{
			pose.orientation[index] = first[index] + sign * second[index];
			length += (double)pose.orientation[index] * pose.orientation[index];
		}
		length = std::sqrt(length);
		for (int index = 0; index < 4; ++index)
		{
			pose.orientation[index] = length > 1e-9 ? (float)(pose.orientation[index] / length) : views[0].pose.orientation[index];
		}
		for (int axis = 0; axis < 3; ++axis)
		{
			pose.position[axis] = (views[0].pose.position[axis] + views[1].pose.position[axis]) * 0.5f;
		}
	}
	// Its axes in the world (the head's tilted less than VrLevelRollDegrees made level, as the eyes are drawn: see
	// FirstPersonView::levelSmallTilt), and its place: the head's eye (see vrPlaceEye) and the pose's offset from the head.
	double up[3];
	vrAxes(pose.orientation, frame.ahead, frame.right, up);
	if (!hand)
	{
		FirstPersonView::levelSmallTilt(frame.ahead, frame.right, up);
	}
	const double fromHeadInTracking[3] = { (double)pose.position[0] - _vrHead.position[0], (double)pose.position[1] - _vrHead.position[1],
		(double)pose.position[2] - _vrHead.position[2] };
	double fromHead[3];
	vrToWorld(fromHeadInTracking, fromHead);
	for (int axis = 0; axis < 3; ++axis)
	{
		frame.down[axis] = -up[axis];
		frame.origin[axis] = _vrSoldierEye[axis] + _vrHeadOffset[axis] + fromHead[axis] / FirstPerson::VrMetresPerVoxel / (axis == 2 ? VrVoxelHeight : 1.0);
	}
}

/**
 * Makes the HUD's own picture for this frame (see _vrHud): the screen's size, transparent (index
 * 0) where empty, in the state's palette; our overlays as the screen shows them (see drawOverlays:
 * without the crosshair, which the eyes draw at its depth), and the game's own warning and debug
 * line, where the 3D view puts them.
 */
void FirstPersonMode::vrComposeHud()
{
	SDL_Surface *screen = _game->getScreen()->getSurface();
	if (!_vrHud || _vrHud->getWidth() != screen->w || _vrHud->getHeight() != screen->h)
	{
		delete _vrHud;
		_vrHud = new Surface(screen->w, screen->h);
	}
	std::copy(_state->getPalette(), _state->getPalette() + 256, _vrHudPalette);
	_vrHud->setPalette(_vrHudPalette);
	_vrHud->clear();
	drawOverlays(_vrHud->getSurface(), true, false);
	if (isActive() && !_classicHud)
	{
		_state->_warning->blit(_vrHud->getSurface());
		_state->_txtDebug->blit(_vrHud->getSurface());
	}
}

/**
 * The HUD as its own picture (see vrComposeHud), as the last frame left it.
 * @param picture Set to it (no field of view: the shell places its quad, VrHudWidthDegrees wide).
 * @return False with nothing to show (VR mode off, the 3D view hidden, no picture made yet).
 */
bool FirstPersonMode::vrHudPicture(FirstPerson::VrPicture &picture)
{
	picture = FirstPerson::VrPicture();
	if (!_vrHud || !vrStereoReady())
	{
		return false;
	}
	SDL_Surface *hud = _vrHud->getSurface();
	picture.pixels = (const Uint8 *)hud->pixels;
	picture.width = hud->w;
	picture.height = hud->h;
	picture.pitch = hud->pitch;
	picture.palette = _vrHudPalette;
	return true;
}

/**
 * The desktop's VR preview (the setting's third value: a debug view, for testing VR without a
 * headset): the stand-in's two eyes drawn (see vrRenderEyes, at the setting's size) and laid side
 * by side on the screen, each in its half, as big as fits (nearest pixel), on the panels' colour;
 * over each the HUD's picture at its depth: a quad VrHudWidthDegrees wide, VrHudDistance ahead of
 * the head, as that eye sees it (centred on its view axis, moved by the eye's own offset from the
 * head's middle: the parallax that puts it at that depth). The whole screen: nothing else is drawn.
 * @param screen The screen.
 */
void FirstPersonMode::vrBlitPreview(SDL_Surface *screen)
{
	FirstPerson::VrEyeView views[2];
	vrStandInViews(views);
	int width, height;
	FirstPerson::vrEyeSize(width, height);
	FirstPerson::VrPicture pictures[2];
	// (OPENXCOM3D_GPU_VR_CARD=check: with "Renderer": GPU each eye is also finished on the card as a headset's display
	// would take it, read back and compared with the picture shown here; the log has the counts.)
	static const char *const cardAsked = getenv("OPENXCOM3D_GPU_VR_CARD");
	static const bool cardCheck = cardAsked && !strcmp(cardAsked, "check");
	static int cardSlot = 0;
	cardSlot = (cardSlot + 1) % 3;
	if (!vrRenderEyes(views, width, height, pictures, nullptr, cardCheck ? cardSlot : -1, cardCheck))
	{
		return;
	}
	vrComposeHud();
	const SDL_Surface *hud = _vrHud->getSurface();

	// Each eye's half: as big as fits, side by side in the middle, on the panels' colour.
	Uint8 backdrop, border;
	panelColors(backdrop, border);
	SDL_FillRect(screen, nullptr, backdrop);
	const int halfWidth = screen->w / 2;
	const double scale = std::min(halfWidth / (double)width, screen->h / (double)height);
	const int shownWidth = (int)(width * scale), shownHeight = (int)(height * scale);
	const int top = (screen->h - shownHeight) / 2;
	const double headRight[3] = { 1.0, 0.0, 0.0 };
	double rightInTracking[3];
	rotateByQuaternion(_vrHead.orientation, headRight, rightInTracking);
	SDL_LockSurface(screen);
	for (int eye = 0; eye < 2; ++eye)
	{
		const FirstPerson::VrPicture &picture = pictures[eye];
		const int left = eye * halfWidth + (halfWidth - shownWidth) / 2;
		// The eye's picture, nearest pixel.
		for (int y = 0; y < shownHeight; ++y)
		{
			const Uint8 *sourceRow = picture.pixels + std::min(picture.height - 1, (int)(y / scale)) * picture.pitch;
			Uint8 *screenRow = (Uint8 *)screen->pixels + (top + y) * screen->pitch + left;
			for (int x = 0; x < shownWidth; ++x)
			{
				screenRow[x] = sourceRow[std::min(picture.width - 1, (int)(x / scale))];
			}
		}
		// The HUD at its depth, on this eye: its picture's pixels a degree, where its view axis is, and the eye's offset
		// from the head's middle along the head's right (metres).
		const double tangentLeft = std::tan(picture.fov.angleLeft), tangentRight = std::tan(picture.fov.angleRight), tangentUp = std::tan(picture.fov.angleUp);
		const double focal = picture.width / std::max(1e-6, tangentRight - tangentLeft);
		const double axisX = -tangentLeft * focal, axisY = tangentUp * focal;
		double offset = 0.0;
		for (int axis = 0; axis < 3; ++axis) offset += ((double)views[eye].pose.position[axis] - _vrHead.position[axis]) * rightInTracking[axis];
		const double hudHalfWidth = focal * std::tan(FirstPerson::VrHudWidthDegrees * Pi / 360.0);
		const double hudHalfHeight = hudHalfWidth * hud->h / hud->w;
		const double hudCentreX = axisX - focal * offset / FirstPerson::VrHudDistance, hudCentreY = axisY;
		// ...on the screen: its rectangle in this eye's half, each of its screen pixels from the HUD's picture (nearest).
		const double hudLeft = left + (hudCentreX - hudHalfWidth) * scale, hudTop = top + (hudCentreY - hudHalfHeight) * scale;
		const double hudShownWidth = 2.0 * hudHalfWidth * scale, hudShownHeight = 2.0 * hudHalfHeight * scale;
		const int firstX = std::max(left, (int)std::ceil(hudLeft)), endX = std::min(left + shownWidth, (int)std::floor(hudLeft + hudShownWidth));
		const int firstY = std::max(top, (int)std::ceil(hudTop)), endY = std::min(top + shownHeight, (int)std::floor(hudTop + hudShownHeight));
		for (int y = firstY; y < endY; ++y)
		{
			const int hudY = std::min(hud->h - 1, (int)((y - hudTop) / hudShownHeight * hud->h));
			const Uint8 *hudRow = (const Uint8 *)hud->pixels + hudY * hud->pitch;
			Uint8 *screenRow = (Uint8 *)screen->pixels + y * screen->pitch;
			for (int x = firstX; x < endX; ++x)
			{
				const Uint8 color = hudRow[std::min(hud->w - 1, (int)((x - hudLeft) / hudShownWidth * hud->w))];
				if (color) screenRow[x] = color;
			}
		}
	}
	SDL_UnlockSurface(screen);
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
