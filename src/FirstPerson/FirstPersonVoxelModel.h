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
 * FirstPersonVoxelModel.h: items given volume. An item's inventory picture (the game's own,
 * from BIGOBS.PCK) becomes a small voxel model, cushion style: each picture pixel a column of
 * voxels in depth, deepest in the middle; or a blade's, a flat plate one voxel thick. The
 * weapon or other item in the soldier's hand (FirstPersonMode) and an item thrown through the
 * air (FirstPersonView) are drawn from it.
 */
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

class Surface;
class RuleFirstPerson;
class RuleItem;

/// One face of a voxel model, between a voxel and the air: its four corners in picture x, y and
/// depth (depth centred on 0), wound so that the cross product of its first two edges points
/// outwards, and its palette colour.
struct VoxelFace { float corner[4][3]; Uint8 color; };

/// Gives a picture volume: the faces of its voxel model (none for an empty picture), a cushion or a flat plate.
void voxelizePicture(Surface *picture, std::vector<VoxelFace> &faces, bool plate = false);
/// Is an item's picture given volume as a flat plate (a blade: a sword, a knife, an axe) rather than a cushion?
bool pictureIsPlate(const RuleFirstPerson &table, const RuleItem *item, Surface *picture);

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
