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
/// (VOXELSCALE: an experiment, off in the game: 2 makes bodies from frames enlarged twice, on cells half as big; then
/// everything the builder gives is in half voxels. See tools/voxel_units.)
#ifndef VOXELSCALE
#define VOXELSCALE 1
#endif
constexpr int FrameSize = 64 * VOXELSCALE;
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
/// the big walkers' once a footfall). Still: the unit can't move (its armour's allowsMoving: false; set for the unit
/// being made, not in the routines' table): a big one is a turret, mounted on a vehicle. Beast: a mod's rules name the
/// unit a long body on four legs (a dog, a cat: the unit table), made from its side drawings if it's drawn on one
/// frame (set for the unit being made too; see longBody). Only a named unit is made so. TorsoDepth and backTop: what
/// the unit table says of a named unit's torso and of how what faces up is painted (see headAndTorso, makeModel); 0 and
/// false for a unit it doesn't name. In every pose a body is as its drawings
/// have it (the owner: "voxel models should generally follow the sprites as not to change the game engine underneath
/// and playstyle"): a kneeling body is as high as its kneeling drawing, not pressed to the game's kneeling height.
/// Frontal: a named unit's body keeps to its front and back drawings' outlines strictly (see makeModel).
/// TankHull, tankTop: for a tank (a hull with a turret of its own, see makeModel), how high its deck and its turret's
/// top stand, voxels: a kneeling soldier's height by the game's rules, and 20 (set for the unit being made; 0: as its
/// drawings have it). The one body not as high as its sprite: the owner's ruling on GitHub issue #201.
/// Arms: what the unit table says of a named unit's arms, a bit each: 1 outside the body, 2 coloured along their
/// length, 4 painted by their own drawing alone, 8 posed: an arm is where its bones are, and what its drawings add
/// away from them is left out (see makeModel).
/// Traits: more the unit table says of a named unit, a bit each: 1 its trunk is round ("trunk: round": a long body as
/// broad as it is tall, whatever its frontal drawings show; see profile), 2 its head is its face and its hair ("head:
/// face": only what is turned to the front is the front drawing's, the rest of the head the back drawing's; see
/// makeModel).
/// Kneeling: the pose the body is made in (set for the unit being made). Standard: the unit table names it a person
/// ("humanoid: standard"): its body is cut by its front, back and side drawings alone, painted by the drawing each
/// cube is turned to, holds no item if it is drawn on one frame, and is made the standard humanoid (see
/// standardBody). Only a named unit is: every other body stays as its drawings alone leave it. Sides: the unit table
/// names it "sculpt: sides": a big walker cut by its four drawings from front, back and sides (see makeModel).
/// Relief: the unit is drawn the same from every side, and lopsided (set for the unit being made, see
/// VoxelUnits::modelFor): its one drawing given volume is its body (see reliefModel).
struct Routine { int number; bool whole, humanoid, legsSwing, armsSwing; int bob[8]; bool big = false; bool still = false; bool beast = false; float torsoDepth = 0.0f; bool backTop = false; bool frontal = false; bool relief = false; int arms = 0; float tankHull = 0.0f, tankTop = 0.0f; bool kneeling = false; bool standard = false; bool sides = false; int traits = 0; };
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
/// Melee: a weapon to strike with (the game's battle type): it points the way its hand frames draw it (see placeItem).
struct HeldItem
{
	std::vector<Uint8> picture;
	int width = 0, height = 0;
	bool twoHanded = false;
	bool melee = false;
	/// How long it is for its hand drawing (see VoxelUnits::handShare): 1, or less for a melee weapon that shares its
	/// hand drawing with longer ones (a mod's knife drawn in the hand as its machete is).
	float share = 1.0f;
};

/// Makes a unit's body from its parts' frames (and, for a unit drawn in parts, the parts laid over each other as the game
/// draws them, each direction's: which part shows where), the items in its hands (right, left), its death frames, its
/// corpse's picture on the map's floor and in the inventory (lying) and its walk frames; null if it has no torso.
std::unique_ptr<VoxelUnitModel> makeModel(PartFrames parts[PartCount], const std::vector<Uint8> composite[8], const HeldItem items[2],
	const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse, const HeldItem &lying, const WalkFrames &walks, bool aiming, const Routine &routine,
	const SDL_Color *palette);

/// Writes a body into files named base and these endings (the viewers in tools/voxel_units read them): .points (each
/// voxel: right ahead up part, its 16 colours, the views it shows in and 1 for a piece added for a view), .walk (each
/// walk phase's places) and .fallN.points (each of the fall's shapes, as a body).
void writeModel(const VoxelUnitModel &model, const std::string &base);

}

}
