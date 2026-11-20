/*
 * FirstPersonVoxelBodiesFine.cpp: the voxel bodies' builder a second time, twice as fine (see
 * FirstPersonVoxelBodiesFine.h): the same source, FirstPersonVoxelBodies.cpp, with VOXELSCALE 2 and under a name of its
 * own, so both builders are in the game; and the way in, which enlarges the unit's frames twice first.
 */
#define VOXELSCALE 2
#define VoxelBodies VoxelBodiesFine
#include "FirstPersonVoxelBodies.cpp"
#undef VoxelBodies
#include "FirstPersonVoxelBodiesFine.h"

namespace OpenXcom
{

namespace
{

/// A 64 pixel frame enlarged twice (Scale2x on the palette's indices: an edge between two colours stays an edge, a
/// diagonal one is smoothed; no new colours). An empty frame stays empty.
std::vector<Uint8> enlarged(const std::vector<Uint8> &frame)
{
	const int small = VoxelBodiesFine::FrameSize / 2, big = VoxelBodiesFine::FrameSize;
	if (frame.size() != (size_t)small * small) return std::vector<Uint8>();
	std::vector<Uint8> out((size_t)big * big);
	auto at = [&](int x, int y) { return x < 0 || y < 0 || x >= small || y >= small ? (Uint8)0 : frame[y * small + x]; };
	for (int y = 0; y < small; ++y)
	{
		for (int x = 0; x < small; ++x)
		{
			const Uint8 e = at(x, y), b = at(x, y - 1), d = at(x - 1, y), f = at(x + 1, y), h = at(x, y + 1);
			Uint8 e0 = e, e1 = e, e2 = e, e3 = e;
			if (b != h && d != f)
			{
				e0 = d == b ? d : e; e1 = b == f ? f : e; e2 = d == h ? d : e; e3 = h == f ? f : e;
			}
			out[(2 * y) * big + 2 * x] = e0; out[(2 * y) * big + 2 * x + 1] = e1;
			out[(2 * y + 1) * big + 2 * x] = e2; out[(2 * y + 1) * big + 2 * x + 1] = e3;
		}
	}
	return out;
}

}

/**
 * Makes a body twice as fine: the frames enlarged twice, the fine builder's body, its places marked as half voxels.
 */
std::unique_ptr<VoxelUnitModel> makeFineModel(const FineBodySource &source, const SDL_Color *palette)
{
	using namespace VoxelBodiesFine;
	const Routine *routine = routineFor(source.routine);
	if (!routine) return nullptr;
	PartFrames parts[PartCount];
	for (int p = 0; p < PartCount; ++p)
	{
		for (int d = 0; d < 8; ++d)
		{
			parts[p].frame[d] = enlarged(source.parts[p][d]);
			if (parts[p].frame[d].empty()) parts[p].frame[d].assign((size_t)FrameSize * FrameSize, 0);
			for (Uint8 colour : parts[p].frame[d]) parts[p].any |= colour != 0;
		}
	}
	std::vector<Uint8> composite[8];
	for (int d = 0; d < 8; ++d) composite[d] = enlarged(source.composite[d]);
	HeldItem items[2];
	for (int hand = 0; hand < 2; ++hand)
	{
		items[hand].picture = source.itemPictures[hand];
		items[hand].width = source.itemWidth[hand];
		items[hand].height = source.itemHeight[hand];
		items[hand].twoHanded = source.itemTwoHanded[hand];
		items[hand].melee = source.itemMelee[hand];
	}
	std::vector<std::vector<Uint8>> deaths;
	for (const auto &death : source.deaths) deaths.push_back(enlarged(death));
	const std::vector<Uint8> corpse = enlarged(source.corpse);
	HeldItem lying;
	lying.picture = source.lyingPicture;
	lying.width = source.lyingWidth;
	lying.height = source.lyingHeight;
	WalkFrames walks;
	walks.any = source.walkAny;
	for (int phase = 0; phase < 8; ++phase)
	{
		for (int view = 0; view < 2; ++view)
		{
			walks.legs[phase][view] = enlarged(source.walkLegs[phase][view]);
			for (int side = 0; side < 2; ++side) walks.arms[phase][view][side] = enlarged(source.walkArms[phase][view][side]);
		}
	}
	Routine made = *routine;
	made.still = source.still;
	made.beast = source.beast;
	made.torsoDepth = source.torsoDepth;
	made.backTop = source.backTop;
	made.frontal = source.frontal;
	made.relief = source.relief;
	made.arms = source.arms;
	std::unique_ptr<VoxelUnitModel> model = makeModel(parts, composite, items, deaths, corpse, lying, walks, source.aiming, made, palette);
	if (model) model->unit = 1.0f / VOXELSCALE;
	return model;
}

}
