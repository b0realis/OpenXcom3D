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
 * FirstPersonSpriteHits.h: setting "Full sprite hitboxes", the pictures the lines of fire test
 * (see FirstPersonSpriteHits.cpp): shared with the 3D view, which outlines what they test
 * (setting "Hitboxes"), and with the aim under the crosshair.
 */
#include <vector>
#include <SDL_types.h>
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class BattleUnit;
class SavedBattleGame;
class FirstPersonView;
class Surface;

namespace FirstPerson
{

/**
 * A unit's picture as a line of fire tests it (setting "Full sprite hitboxes"): a flat card in
 * the world with the unit's sprite on it, its frame the one seen from where the line comes from.
 * The card stands where the 3D view draws the picture: the middle of the unit's tiles, its feet
 * on the floor's top, at the setting "Unit size". For the lines of the soldier looked through in
 * 3D (his crosshair, his shots) it is the very card the view draws, leaning as it leans; for
 * everyone else's it stands upright, facing the line's start.
 */
struct SpriteCard
{
	const BattleUnit *unit = nullptr;
	double anchor[3] = {};    // the picture's anchor pixel in the world (the floor diamond's middle, at the feet), in voxels
	double column[3] = {};    // one picture column to the right, in voxels
	double row[3] = {};       // one picture row up, in voxels
	double normal[3] = {};    // the card's unit normal
	int anchorX = 32, anchorY = 40; // the anchor pixel in the picture
	bool rowsBelow = false;   // its art below the feet counts too (a unit in the air, drawn standing all of it); else that lies on the floor, never hit
	const std::vector<Uint8> *picture = nullptr; // SpriteBufferSize square, 1 for a solid pixel
	const Surface *smooth = nullptr; // the same frame as the 3D view draws it smooth up close (setting "Sprite upscaling"), if it does: tested instead
	int factor = 1;                  // how many times bigger that is
};

/**
 * The card that the lines from a 3D view's eye test for a unit, as that view would see it hit:
 * the main view's own (as it draws it, see SpriteCard) while it shows the battle, else upright
 * facing the view's eye. Used to outline what is tested (setting "Hitboxes").
 * @param save The battle.
 * @param unit The unit.
 * @param view The view.
 * @param card Set to the card.
 * @return False if the unit has no card (down, or none to make).
 */
bool viewCard(SavedBattleGame *save, const BattleUnit *unit, const FirstPersonView *view, SpriteCard &card);

/**
 * The side a unit's frame is drawn from in a 3D view, and so the picture its card carries (the
 * isometric art shows units from the south-east; see UnitSprite::setViewRotation): the side
 * turned towards the eye; straight above or below the unit (within a voxel of it across the
 * map), where no side is turned towards it, from the south-east, as the map shows the unit (a
 * player, GitHub issue #112: shooting straight down at a unit, the shot's shake tipped the eye
 * from side to side over it and the unit spun round and twitched). The view draws it so and the
 * lines of fire test that very picture (setting "Full sprite hitboxes"), both from the view's
 * steady eye (FirstPersonView::getSteadyEye).
 * @param eye The eye, in voxels.
 * @param feet The unit's feet (its picture's anchor), in voxels.
 * @return The view rotation (3 minus the direction from the unit to the eye).
 */
int unitViewRotation(const double eye[3], const double feet[3]);

/**
 * Where a line crosses a card's plane, and whether a solid pixel of the picture is there (the
 * art below the feet only for a unit drawn standing in the air). Pure: safe on any thread.
 * @param card The card.
 * @param start The line's start, in voxels.
 * @param direction Its direction (a unit vector), in voxels.
 * @param along Set to how far along the line the crossing is, in voxels.
 * @return True if the line crosses the card on a solid pixel.
 */
bool cardRay(const SpriteCard &card, const double start[3], const double direction[3], double &along);

/**
 * The aim under the crosshair (FirstPersonMode::aimAt) walks the game's line from the voxel the
 * eye is in; while one of these lives, the pictures that line meets are tested along the eye's
 * exact ray instead (the line's start can be half a voxel off the eye each way), so the crosshair
 * finds a unit exactly where its picture shows on the screen.
 */
class ExactLine
{
public:
	/// Starts the exact ray for lines walked from that voxel.
	ExactLine(const double eye[3], const double direction[3], const Position &start);
	/// Ends it.
	~ExactLine();
};

/**
 * Where the last exact line (see ExactLine) met a unit's picture, exactly.
 * @param point Set to the point, in voxels.
 * @return False if it met none.
 */
bool lastExactCrossing(double point[3]);

}
}
