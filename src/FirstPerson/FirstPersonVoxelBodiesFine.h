#pragma once
/*
 * FirstPersonVoxelBodiesFine.h: the voxel bodies made twice as fine (see FineBodies): the builder of
 * FirstPersonVoxelBodies.cpp built a second time (FirstPersonVoxelBodiesFine.cpp) to work on the unit's frames enlarged
 * twice, on cells half as big: a quarter of a voxel each. What it gives is a body like any other whose places are in
 * half voxels (VoxelUnitModel::unit 0.5).
 */
#include <memory>
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

struct VoxelUnitModel;

/// What a fine body is made from, as the builder's job has it (see VoxelUnits::Job), in plain lists: each part's frame
/// for each direction (64 pixels square), the parts laid over each other, the two items' pictures with their sizes and
/// whether each is held in both hands, the death frames and the corpse's picture, the walk's frames (legs, then each
/// arm's, by phase and view).
struct FineBodySource
{
	std::vector<Uint8> parts[6][8], composite[8], itemPictures[2];
	int itemWidth[2] = {}, itemHeight[2] = {};
	bool itemTwoHanded[2] = {};
	std::vector<std::vector<Uint8>> deaths;
	std::vector<Uint8> corpse, lyingPicture;
	int lyingWidth = 0, lyingHeight = 0;
	std::vector<Uint8> walkLegs[8][2], walkArms[8][2][2];
	bool walkAny = false, aiming = false;
	int routine = 0;
	bool still = false; // (it can't move: see Routine)
};

/// Makes a body twice as fine from a unit's frames; null if it has no torso (or its routine has no bodies).
std::unique_ptr<VoxelUnitModel> makeFineModel(const FineBodySource &source, const SDL_Color *palette);

}
