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
	bool itemMelee[2] = {};
	float itemShare[2] = { 1.0f, 1.0f }; // (how long each is for its hand drawing: see HeldItem::share)
	std::vector<std::vector<Uint8>> deaths;
	std::vector<Uint8> corpse, lyingPicture;
	int lyingWidth = 0, lyingHeight = 0;
	std::vector<Uint8> walkLegs[8][2], walkArms[8][2][2];
	bool walkAny = false, aiming = false;
	int routine = 0;
	bool still = false; // (it can't move: see Routine)
	bool beast = false; // (a long body on four legs, by the mod's rules: see Routine)
	float torsoDepth = 0.0f; // (the torso's slices at most this deep for their width, by the mod's rules: see Routine)
	bool backTop = false; // (what faces up painted by the back drawing, by the mod's rules: see Routine)
	bool frontal = false; // (kept to its front and back drawings' outlines, by the mod's rules: see Routine)
	bool kneeling = false; // (the pose it is made in: see Routine)
	bool standard = false; // (a person, made the standard humanoid, by the mod's rules: see Routine)
	bool sides = false; // (a big walker cut by its four drawings from front, back and sides, by the mod's rules: see Routine)
	int traits = 0; // (a round trunk, a head of face and hair, by the mod's rules: see Routine)
	bool relief = false; // (its one drawing given volume: see Routine)
	int arms = 0; // (its arms outside the body, coloured along their length, by the mod's rules: see Routine)
	float tankHull = 0.0f, tankTop = 0.0f; // (a tank's deck and top, voxels: see Routine)
	float flatTop = 1.0f, flatUnder = 1.0f; // (pressed flatter above and under its widest level, by the mod's rules: see Routine)
};

/// Makes a body twice as fine from a unit's frames; null if it has no torso (or its routine has no bodies).
std::unique_ptr<VoxelUnitModel> makeFineModel(const FineBodySource &source, const SDL_Color *palette);

}
