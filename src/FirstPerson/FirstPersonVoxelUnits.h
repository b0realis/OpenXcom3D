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
 * FirstPersonVoxelUnits.h: units given a body. A unit's own sprites (the soldiers, aliens and
 * civilians of both games, one tile big or two by two; its drawing routine draws it in parts, legs,
 * a torso with the head, two arms, the items in hand, or all of it on one frame, for each of the 8
 * directions) become a voxel model: the head and torso sliced from the frontal sprite's width and
 * the side sprite's depth, the arms and legs (or the whole body) likewise, each part then carved by
 * its 8 frames, an arm given a skeleton too, and every surface voxel coloured by the frame of the
 * direction nearest the camera's. An item in hand is its inventory picture given volume, where the
 * game's frames of it in the hand put it. The body walks as its walk frames show (its legs and free
 * arms swung to match them), goes down as its death frames show and lies as its corpse's picture.
 * Setting "Voxel units" (off: the game's pictures, untouched); the 3D view draws them
 * (FirstPersonViewSprites.cpp), the game's flames and bubbles over them.
 */
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

class Armor;
class BattleUnit;
class RuleItem;
class SavedBattleGame;
class SurfaceSet;
class UnitSprite;

/// A unit's body made from its own sprites: its surface voxels and their colours, standing (or
/// kneeling, aiming) as the unit was when it was made, and the walk's 8 poses.
struct VoxelUnitModel
{
	/// The unit is drawn as a dither, every other pixel left out so the map shows through it (XPiratez's stealth and
	/// spectre suits): its body is made whole and drawn see-through (see VoxelDitherFaint).
	bool dithered = false;
	/// Which part of the unit a voxel is.
	enum Part : Uint8 { Torso = 1, LeftArm, RightArm, Legs, Item };
	/// Each surface voxel's place from the middle of the unit's feet: to its right, ahead and up, in voxels.
	std::vector<float> right, ahead, up;
	/// How many voxels one of those is: 1, or 0.5 for a body made twice as fine (see FineBodies; the walk,
	/// the fall's edges and the hits are in the same measure).
	float unit = 1.0f;
	/// Which part each belongs to.
	std::vector<Uint8> part;
	/// What the sprites show at each voxel: 16 palette colours a voxel, the 8 directions' frames painted
	/// level (as a picture taken at the unit's height, for a camera near its height) and then the same
	/// painted along the isometric view's slant (for a camera high above).
	std::vector<Uint8> colours;
	/// The walk: each of its 8 phases' places (the same voxels, legs and arms swung, the body bobbing).
	std::vector<float> walkRight[8], walkAhead[8], walkUp[8];
	/// A shape's cubes taken coarser, for drawing it far away on the processor (see FirstPersonView::recordVoxelUnit):
	/// one cube for each block of 2 (or 4) cells each way that has any, the cube standing for it and how far the
	/// block's middle is from it (right, ahead, up). Made the first time it is drawn so, by the drawing thread alone.
	struct Coarse
	{
		std::vector<Uint32> voxel;
		std::vector<float> shift;
		bool made = false;
	};
	mutable Coarse coarse[2];
	/// Each cube's bare faces, a bit each (to its right, left, ahead, behind, up, down: no cube of the shape beside it
	/// there), for the processor to leave out the cubes that show the eye none of them (see recordVoxelUnit). Made the
	/// first time the shape is drawn, by the drawing thread alone; what it holds has every bit.
	mutable std::vector<Uint8> bare;
	/// A shape of the body going down or lying: cubes on the body's own grid, each one colour from every side.
	struct FallShape
	{
		mutable Coarse coarse[2];
		mutable std::vector<Uint8> bare;
		std::vector<float> right, ahead, up;
		std::vector<Uint8> colour;
		/// What a shot meets of it (see VoxelUnitModel::hits): its cubes, in whole voxels from hitFrom, hitSize each way.
		std::vector<Uint8> hits;
		int hitFrom[3] = {}, hitSize[3] = {};
		/// Whether a shot meets it at a point (voxels to the body's right, ahead and up from the middle of its feet).
		bool hitAt(float across, float along, float height) const;
	};
	/// Going down as its death frames show it: each frame's own picture given volume (the unit collapses as its sprite
	/// does, falling back from the way it faced), then the body lying: its corpse's inventory picture laid on the floor
	/// and given volume. Empty for a unit with neither.
	std::vector<FallShape> falls;
	/// The shape at a point of the fall (0 standing, 1 down), or null (none: before it starts, or a body without any).
	const FallShape *fallAt(double progress) const;
	/// The colour of voxel k seen from direction (0-7) with level painting (or slanted).
	Uint8 colour(size_t k, int direction, bool level) const { return colours[k * 16 + (level ? 0 : 8) + direction]; }
	/// The views each voxel shows in, a bit for each of the 16 colours' views (level, then slanted: the bit of colour slot
	/// (level ? 0 : 8) + direction): the hand drawn frames don't quite agree on one shape, so seen from one of the 8
	/// directions the body shows only its voxels inside that direction's drawing, and pieces are added for what the
	/// drawing has that they leave bare (see matchViews in FirstPersonVoxelBodies.cpp): from each direction the body
	/// has its sprite's outline.
	std::vector<Uint16> views;
	/// The voxels from addedFrom on are such added pieces.
	size_t addedFrom = (size_t)-1;
	/// Whether voxel k shows from direction (0-7) with level painting (or slanted).
	bool shows(size_t k, int direction, bool level) const { return views.empty() || (views[k] >> ((level ? 0 : 8) + direction) & 1); }
	/// What a shot meets (settings "Voxel units" and "Full sprite hitboxes": the body is the hitbox): the body's solid
	/// voxels, whole voxels (right, ahead, up) from hitFrom, hitSize of them each way, each the views it shows in (as
	/// views; 0: empty).
	std::vector<Uint16> hits;
	int hitFrom[3] = {}, hitSize[3] = {};
	/// Whether a shot meets the body at a point (voxels to the body's right, ahead and up from the middle of its feet)
	/// coming from direction (0-7) at the body's height (level) or from above.
	bool hitAt(float across, float along, float height, int direction, bool level) const;
};

/// Whether the bodies are made fine: voxels half the size, from the frames enlarged with their edges kept (see
/// makeFineModel). Not on a headset: four times the voxels to draw, for each eye.
#ifdef __ANDROID__
constexpr bool FineBodies = false;
#else
constexpr bool FineBodies = true;
#endif

/// The voxel bodies of the units seen so far in a battle, one for each unit and pose. A body is made
/// on a thread of its own (a few to a hundred milliseconds; the thread started with the first), from
/// the frames captured on the game's; the unit is drawn as its sprite until its body is ready.
class VoxelUnits
{
public:
	VoxelUnits();
	~VoxelUnits();
	/// Whether a unit can be drawn as a voxel body: of a size and drawn by a routine the bodies know
	/// (see Routine), and up.
	static bool suits(const BattleUnit *unit);
	/// Whether a unit that is down can have a body made as it stood, for its corpse to lie as.
	static bool suitsDown(const BattleUnit *unit);
	/// The body for a unit as it is now, made the first time from its sprites (drawn with the unit
	/// sprite given, which is left turned to the unit's own direction, its other parts drawn nowhere)
	/// its items' inventory pictures (BIGOBS.PCK) and its corpse's (FLOOROB.PCK), in the battle's palette, while builds (counted
	/// down: frames captured for a body) allows; null when there's none (yet: one being made; or
	/// walking before it was ever seen standing). walkPhase: out, the walk's phase to draw (or -1).
	const VoxelUnitModel *modelFor(BattleUnit *unit, UnitSprite &unitSprite, const SurfaceSet *inventorySprites, SurfaceSet *floorSprites,
		const SavedBattleGame *save, const SDL_Color *palette, int &builds, int &walkPhase);
	/// The body a unit was last drawn with (for its fall, and its corpse lying where it fell), or null.
	const VoxelUnitModel *lastBody(const BattleUnit *unit) const;
	/// The way a unit faced when it was last drawn as a body, up.
	int lastFacing(const BattleUnit *unit) const;
	/// The body for a unit as it is now, if it's made (none is made here: see modelFor), or null; a unit walking, its
	/// standing body.
	const VoxelUnitModel *bodyNow(const BattleUnit *unit);
	/// The bodies are for this battle: every body forgotten if they were another's.
	void forBattle(const SavedBattleGame *save);
	/// Forgets every body, and those being made (a new battle).
	void clear();
private:
	/// The frames and pictures a body is being made from (defined with the builder).
	struct Job;
	/// What a body is made for: the unit, its armour, kneeling, aiming, flying, what it holds, a big unit's turret's turn.
	typedef std::tuple<int, const Armor*, bool, bool, bool, const RuleItem*, const RuleItem*, int> Key;
	std::map<Key, std::shared_ptr<VoxelUnitModel>> _models;
	/// The bodies by what they were made from (their frames and pictures, hashed): units drawn alike share one.
	std::unordered_map<Uint64, std::shared_ptr<VoxelUnitModel>> _byFrames;
	/// The key for a unit as it is now; standing: as it would stand (for the walk).
	static Key keyFor(const BattleUnit *unit, bool standing);
	/// The body each unit was last drawn with (by its id).
	std::map<int, std::shared_ptr<VoxelUnitModel>> _lastBodies;
	std::map<int, int> _lastFacings;
	/// Notes a unit's body as the one it was last drawn with; gives the body.
	const VoxelUnitModel *remember(const BattleUnit *unit, const std::shared_ptr<VoxelUnitModel> &body);
	/// The bodies being made (by what they're made from), and the keys waiting for one.
	std::set<Uint64> _making;
	std::map<Key, Uint64> _waiting;
	/// The builder's jobs, the bodies it has made, and which battle they're for (a new battle drops the old one's); and
	/// its threads (last: the rest is ready before they start; one starts with each job waiting, up to builderThreads).
	std::mutex _mutex;
	std::condition_variable _wake;
	std::deque<std::unique_ptr<Job>> _jobs;
	std::vector<std::pair<Uint64, std::shared_ptr<VoxelUnitModel>>> _made;
	unsigned _battle = 0;
	const SavedBattleGame *_battleSave = nullptr; // (the battle the bodies are for, see forBattle)
	bool _stopping = false;
	size_t _building = 0; // (bodies being made now)
	std::vector<std::thread> _builders;
	/// How many threads make bodies at once.
	static size_t builderThreads();
	/// The builder's loop: makes the bodies queued, until stopped.
	void build();
	/// A developer's aid: writes what a body was made from and what it is into a folder (the environment variable
	/// OXC3D_VOXELDUMP names it; the viewers in tools/voxel_units read the files).
	static void dump(const Job &job, const VoxelUnitModel *model, const char *folder);
	/// Takes the bodies the builder has made.
	void collect();
};

}
// Thanks mom and dad! You were the ones who exposed me to my first comp.
