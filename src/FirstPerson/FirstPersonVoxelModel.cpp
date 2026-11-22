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
 * FirstPersonVoxelModel.cpp: an item's inventory picture made into a voxel model (see the
 * header).
 */
#include "FirstPersonVoxelModel.h"
#include <algorithm>
#include <cmath>
#include "../Engine/Script.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
#include "../Mod/RuleDamageType.h"
#include "RuleFirstPerson.h"

namespace OpenXcom
{

namespace
{

/// A melee weapon whose picture is nowhere further than this from its outline (in steps, see
/// stepsToOutside) is thin enough to be a blade, a flat plate (unless it's blunt).
const int BladeMaxThickness = 4;

/**
 * How many steps each opaque pixel of a picture is from the outside (counting the 8
 * neighbours): the edge pixels are 1, found first, then a breadth-first walk goes inwards from
 * them; transparent pixels are 0.
 * @param picture The picture (palette colours, 0 transparent).
 * @param steps Set to each pixel's steps (index y * width + x).
 */
void stepsToOutside(Surface *picture, std::vector<int> &steps)
{
	const int width = picture->getWidth(), height = picture->getHeight();
	steps.assign(width * height, 0);
	std::vector<int> queue; // pixel indices (y * width + x), walked in order
	auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && picture->getPixel(x, y) != 0; };
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			// An edge pixel has a transparent pixel (or the picture's border) next to it.
			if (!solid(x, y)) continue;
			bool onEdge = false;
			for (int offsetY = -1; offsetY <= 1 && !onEdge; ++offsetY)
				for (int offsetX = -1; offsetX <= 1 && !onEdge; ++offsetX)
					onEdge = !solid(x + offsetX, y + offsetY);
			if (onEdge)
			{
				steps[y * width + x] = 1;
				queue.push_back(y * width + x);
			}
		}
	}
	for (size_t head = 0; head < queue.size(); ++head)
	{
		// Each opaque neighbour not reached yet is one step further in.
		const int x = queue[head] % width, y = queue[head] / width;
		for (int offsetY = -1; offsetY <= 1; ++offsetY)
			for (int offsetX = -1; offsetX <= 1; ++offsetX)
				if (solid(x + offsetX, y + offsetY) && !steps[(y + offsetY) * width + x + offsetX])
				{
					steps[(y + offsetY) * width + x + offsetX] = steps[queue[head]] + 1;
					queue.push_back((y + offsetY) * width + x + offsetX);
				}
	}
}

} // namespace

/**
 * Is an item's picture given volume as a flat plate, one voxel thick (a blade: a sword, a knife,
 * an axe), rather than a cushion? The item table (`firstPerson: items:`, held: plate or
 * cushion) decides first. Otherwise a melee weapon is a plate when it isn't blunt (its damage
 * isn't the clubs' and the stun rods': concussive or stun, high explosive and stun in the
 * original games) and its picture is thin everywhere (BladeMaxThickness): the big mods' swords,
 * knives and axes, not their maces, knuckles or shields.
 * @param table The 3D view's rules (the item table).
 * @param item The item (null: a cushion).
 * @param picture Its inventory picture.
 * @return True for a plate.
 */
bool pictureIsPlate(const RuleFirstPerson &table, const RuleItem *item, Surface *picture)
{
	if (!item)
	{
		return false;
	}
	if (const RuleFirstPerson::ItemLook *look = table.getItemLook(item->getType()))
	{
		if (look->held != RuleFirstPerson::HELD_AUTO) return look->held == RuleFirstPerson::HELD_PLATE;
	}
	const ItemDamageType damage = item->getDamageType()->ResistType;
	if (item->getBattleType() != BT_MELEE || damage == DT_HE || damage == DT_STUN)
	{
		return false;
	}
	std::vector<int> steps;
	stepsToOutside(picture, steps);
	const int thickest = steps.empty() ? 0 : *std::max_element(steps.begin(), steps.end());
	return thickest > 0 && thickest <= BladeMaxThickness;
}

/**
 * Finds what a mod drew into an item's inventory picture for the inventory screen, not for the item. A picture taller
 * than the hand slot (32x48) hangs out under it there, and a mod (the 40k mod, in some 55 pictures of long weapons) draws
 * the screen's grid anew round it: the slot's old bottom line, and any grid line further down, painted over in the
 * background's black, and a new frame in the grid's colour down both sides of the picture to a line along its bottom.
 * Given volume with the item, the frame was a wire loop round the weapon's butt and the painted line a black slab
 * across it (a player's report, GitHub issue #235: "items which exceed the window size act funny").
 * Known by the frame: both side columns of the picture in one colour from under the slot down to a row that is mostly
 * that colour. The paint is the colour most seen on the slot's old line where nothing is drawn over and under it. A
 * pixel of the frame's bottom line or of a painted line across stays where the item is drawn on both sides of it (the
 * item drawn through the line); a painted pixel off those lines goes only as a piece of a line running down, nothing
 * drawn to its left and right.
 * @param picture The picture (palette colours, 0 transparent).
 * @param slotBottom The row the hand slot's bottom line runs along (see handSlotBottom).
 * @param marks Set to 1 for each pixel that is the screen's, 0 for the item's (a row after a row, the picture's size).
 * @return True if the picture has such a frame.
 */
bool slotFrame(const Surface *picture, int slotBottom, std::vector<Uint8> &marks)
{
	const int width = picture->getWidth(), height = picture->getHeight();
	marks.assign(width * height, 0);
	if (width < 4 || slotBottom < 1 || height < slotBottom + 2)
	{
		return false;
	}
	// The frame: its sides, and the lowest row they reach that is three quarters of their colour.
	const Uint8 grid = picture->getPixel(0, slotBottom + 1);
	int bottom = -1;
	for (int y = slotBottom + 1; grid && y < height && picture->getPixel(0, y) == grid && picture->getPixel(width - 1, y) == grid; ++y)
	{
		int same = 0;
		for (int x = 0; x < width; ++x) same += picture->getPixel(x, y) == grid;
		if (same * 4 >= width * 3) bottom = y;
	}
	if (bottom < 0)
	{
		return false;
	}
	// (the item's own drawing: drawn, and not found to be the screen's)
	auto drawn = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && picture->getPixel(x, y) != 0 && !marks[y * width + x]; };
	for (int y = slotBottom + 1; y <= bottom; ++y) marks[y * width] = marks[y * width + width - 1] = 1;
	for (int x = 1; x < width - 1; ++x)
	{
		if (picture->getPixel(x, bottom) == grid && !(drawn(x, bottom - 1) && drawn(x, bottom + 1))) marks[bottom * width + x] = 1;
	}

	// The paint over the old lines.
	int seen[256] = {}, paint = 0;
	for (int x = 1; x < width - 1; ++x)
	{
		const Uint8 colour = picture->getPixel(x, slotBottom);
		if (colour && !drawn(x, slotBottom - 1) && !drawn(x, slotBottom + 1)) ++seen[colour];
	}
	for (int colour = 1; colour < 256; ++colour)
	{
		if (seen[colour] > seen[paint]) paint = colour; // (nothing is counted for 0)
	}
	// (found again while it finds more: a pixel where two painted lines cross has paint on every side at first)
	for (bool more = paint != 0; more; )
	{
		more = false;
		for (int y = slotBottom; y < bottom; ++y)
		{
			const bool across = y == slotBottom || picture->getPixel(1, y) == paint || picture->getPixel(width - 2, y) == paint;
			for (int x = 1; x < width - 1; ++x)
			{
				if (picture->getPixel(x, y) != paint || marks[y * width + x]) continue;
				if ((across && !(drawn(x, y - 1) && drawn(x, y + 1))) || (!drawn(x - 1, y) && !drawn(x + 1, y)))
				{
					marks[y * width + x] = 1;
					more = true;
				}
			}
		}
	}
	return true;
}

/**
 * Clears from an item's inventory picture what a mod drew into it for the inventory screen (see slotFrame).
 * @param picture The picture.
 * @param slotBottom The row the hand slot's bottom line runs along (see handSlotBottom).
 */
void clearSlotFrame(Surface *picture, int slotBottom)
{
	std::vector<Uint8> marks;
	if (!slotFrame(picture, slotBottom, marks))
	{
		return;
	}
	const int width = picture->getWidth(), height = picture->getHeight();
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (marks[y * width + x]) picture->setPixel(x, y, 0);
		}
	}
}

/**
 * Draws an item's inventory picture (BIGOBS.PCK, with the item's own recolouring scripts, as the game draws it in a
 * hand) whole, at the frame's own place and size: a hand slot's (32x48) at least, more where a mod's picture is
 * bigger (a long weapon's, hanging out under the slot: its butt was cut off). The game's hand slot centres the frame
 * by the item's inventory size instead, which pushes a picture drawn wider than its item (a mod's sword lying across
 * the frame, one square wide in the inventory) partly off the frame: its tip was cut off, and the thicker end left
 * looked like the business end. What a mod drew into the picture for the inventory screen is cleared (see slotFrame).
 * @param item The item.
 * @param inventorySprites The game's BIGOBS.PCK.
 * @param save The battle.
 * @param picture Where to draw it: cleared first, and made anew (with the old one's palette) when its size is not the frame's.
 */
void drawItemPicture(const BattleItem *item, const SurfaceSet *inventorySprites, const SavedBattleGame *save, Surface *&picture)
{
	const Surface *frame = inventorySprites ? item->getBigSprite(inventorySprites, save, 0) : nullptr;
	const int width = std::max(HandSlotWidth, frame ? frame->getWidth() : 0), height = std::max(HandSlotHeight, frame ? frame->getHeight() : 0);
	if (!picture || picture->getWidth() != width || picture->getHeight() != height)
	{
		Surface *fresh = new Surface(width, height, 0, 0);
		if (picture && picture->getPalette()) fresh->setPalette(picture->getPalette());
		delete picture;
		picture = fresh;
	}
	picture->clear();
	if (!frame)
	{
		return;
	}
	ScriptWorkerBlit worker;
	BattleItem::ScriptFill(&worker, item, save, BODYPART_ITEM_INVENTORY, 0, 0);
	worker.executeBlit(frame, picture, 0, 0, 0);
	clearSlotFrame(picture, handSlotBottom(item->getRules()->getInventoryHeight()));
}

/**
 * Gives a picture volume, cushion style: each picture pixel becomes a column of voxels in
 * depth, its half depth growing with the square root of its distance to the outline (one voxel
 * at the edge, at most four), so the model is thickest in the middle. Or a blade's, a flat
 * plate: every column one voxel. Only the faces between a voxel and empty space are kept, with
 * corners in picture x, y and depth (depth centred on 0).
 * @param picture The picture (palette colours, 0 transparent).
 * @param faces Set to the model's faces.
 * @param plate A flat plate one voxel thick instead of a cushion.
 */
void voxelizePicture(Surface *picture, std::vector<VoxelFace> &faces, bool plate)
{
	faces.clear();
	const int width = picture->getWidth(), height = picture->getHeight();
	const int MaxHalfDepth = 4, fullDepth = 2 * MaxHalfDepth + 1; // in voxels

	// How many steps each opaque pixel is from the outside (0: transparent).
	std::vector<int> steps;
	stepsToOutside(picture, steps);
	auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && steps[y * width + x] > 0; };

	// Fill the voxel grid (width x height x fullDepth, 0 = empty, else the pixel's colour):
	// each pixel's column reaches its half depth to either side of the middle layer (a plate's:
	// the middle layer only).
	std::vector<Uint8> voxels(width * height * fullDepth, 0);
	auto voxelAt = [&](int x, int y, int z) -> Uint8 {
		return x >= 0 && y >= 0 && z >= 0 && x < width && y < height && z < fullDepth ? voxels[(z * height + y) * width + x] : 0;
	};
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (!steps[y * width + x]) continue;
			const int halfDepth = plate ? 0 : std::clamp((int)std::lround(1.2 * std::sqrt((double)steps[y * width + x])), 1, MaxHalfDepth);
			for (int z = MaxHalfDepth - halfDepth; z <= MaxHalfDepth + halfDepth; ++z) voxels[(z * height + y) * width + x] = picture->getPixel(x, y);
		}
	}

	// A face for every side of a voxel that's open to the air, its corners wound so that the
	// cross product of its first two edges points outwards (the drawing culls by that).
	for (int z = 0; z < fullDepth; ++z)
	{
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const Uint8 color = voxelAt(x, y, z);
				if (!color) continue;
				const int voxel[3] = { x, y, z };

				// The six sides: each axis, towards minus and plus.
				for (int axis = 0; axis < 3; ++axis)
				{
					for (int side = -1; side <= 1; side += 2)
					{
						// Hidden behind a neighbouring voxel: no face.
						int neighbour[3] = { x, y, z };
						neighbour[axis] += side;
						if (voxelAt(neighbour[0], neighbour[1], neighbour[2])) continue;

						// The other two axes, in the order that winds the face outwards.
						const int tangent1 = side > 0 ? (axis + 1) % 3 : (axis + 2) % 3, tangent2 = side > 0 ? (axis + 2) % 3 : (axis + 1) % 3;
						VoxelFace face;
						face.color = color;
						if (axis < 2)
						{
							// A side face (across the picture): the picture's dark outline stays on the
							// front and back faces, and the sides show the colour just inside it.
							const int insideX = x - (axis == 0 ? side : 0), insideY = y - (axis == 1 ? side : 0);
							if (solid(insideX, insideY)) face.color = picture->getPixel(insideX, insideY);
						}

						// The four corners: the voxel spans +-0.5 around its centre, the face lies on
						// its minus or plus side, and the corners go round the two other axes.
						for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
						{
							float cornerPos[3] = { voxel[0] - 0.5f, voxel[1] - 0.5f, voxel[2] - 0.5f - MaxHalfDepth };
							cornerPos[axis] += side > 0 ? 1.0f : 0.0f;
							if (cornerIndex == 1 || cornerIndex == 2) cornerPos[tangent1] += 1.0f;
							if (cornerIndex == 2 || cornerIndex == 3) cornerPos[tangent2] += 1.0f;
							std::copy(cornerPos, cornerPos + 3, face.corner[cornerIndex]);
						}
						faces.push_back(face);
					}
				}
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
