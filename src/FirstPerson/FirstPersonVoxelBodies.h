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
 * FirstPersonVoxelBodies.h: how a unit's voxel body is made (see FirstPersonVoxelUnits.h), from the frames the game's
 * unit drawer gives of each of its parts in the 8 directions, its walk's and death's frames, its corpse's picture and
 * the pictures of the items in its hands. Nothing of the battle is needed: the builder's thread makes bodies with it,
 * and so do the developers' tools (tools/voxel_units: a body made again from the frames the game dumped).
 */
#include <memory>
#include <string>
#include <vector>
#include <SDL.h>
#include "FirstPersonVoxelUnits.h"

namespace OpenXcom
{

namespace VoxelBodies
{

/// The frames are FrameSize pixels square.
constexpr int FrameSize = 64;
/// Direction 3 faces the camera; direction 1 shows the unit's right side (its front to the right),
/// direction 5 its left side (its front to the left).
constexpr int Front = 3, RightSide = 1, LeftSide = 5;
/// The parts a body is made of: the body's own (the first BodyParts: torso, left arm, right arm, legs), then the items
/// in the right and left hands (their frames only say where the hand holds them).
constexpr int PartCount = 6, BodyParts = 4, RightItem = 4, LeftItem = 5;

/// How a unit's drawing routine draws it, for its voxel body: in parts on frames of their own, as routine 0 does (a
/// torso with the head, arms, legs: soldiers, Sectoids, Mutons, TFTD's aquanauts and aliens; the Floater without legs,
/// the Snakeman whose legs are its tail, the Chryssalid), or all of it on one frame (civilians, Ethereals and Zombies,
/// the walkers among them, and the creatures: Silacoid, Celatid, Biodrone, Tentaculat); and how it walks: whether its
/// legs swing (a one-frame walker's: its body below the hip), whether its free arms do, and the torso's bob (the
/// routine's own, as the game draws its walk: drawRoutine0's tables for 0, 10, 13-15, drawRoutine6's for the Snakeman;
/// the big walkers' once a footfall).
struct Routine { int number; bool whole, humanoid, legsSwing, armsSwing; int bob[8]; bool big = false; };
/// The routine's way of drawing, or null for one not given a voxel body.
const Routine *routineFor(int number);

/// One part's frames for the 8 directions (palette colours, 0 empty).
struct PartFrames
{
	std::vector<Uint8> frame[8];
	bool any = false;
	Uint8 at(int direction, int column, int row) const
	{
		return column >= 0 && row >= 0 && column < FrameSize && row < FrameSize ? frame[direction][row * FrameSize + column] : 0;
	}
};

/// The walk as its frames show it (drawn with UnitSprite::setWalkOverride): for each phase and each of the two views it's
/// matched in (WalkViews: the right side, where a leg swings across the view, and the front right, where the near leg
/// and the far one part), the frame of its legs (of all of it, for a body drawn on one frame) and of each arm (right,
/// left). Empty for a unit that doesn't walk in its own frames.
constexpr int WalkViews[2] = { RightSide, 2 };
struct WalkFrames
{
	std::vector<Uint8> legs[8][2], arms[8][2][2];
	bool any = false;
};

/// An item in a hand: its inventory picture, as the game draws it in the hand (palette colours, 0 empty), and how it's held.
struct HeldItem
{
	std::vector<Uint8> picture;
	int width = 0, height = 0;
	bool twoHanded = false;
};

/// Makes a unit's body from its parts' frames (and, for a unit drawn in parts, the parts laid over each other as the game
/// draws them, each direction's: which part shows where), the items in its hands (right, left), its death frames, its
/// corpse's picture and its walk frames; null if it has no torso.
std::unique_ptr<VoxelUnitModel> makeModel(PartFrames parts[PartCount], const std::vector<Uint8> composite[8], const HeldItem items[2],
	const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse, const WalkFrames &walks, bool aiming, const Routine &routine,
	const SDL_Color *palette);

/// Writes a body into files named base and these endings (the viewers in tools/voxel_units read them): .points (each
/// voxel: right ahead up part, its 16 colours, the views it shows in and 1 for a piece added for a view), .walk (each
/// walk phase's places) and .fall (the fall's way, poses, edges and colours).
void writeModel(const VoxelUnitModel &model, const std::string &base);

}

}
