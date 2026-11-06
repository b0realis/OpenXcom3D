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
	/// Which part of the unit a voxel is.
	enum Part : Uint8 { Torso = 1, LeftArm, RightArm, Legs, Item };
	/// Each surface voxel's place from the middle of the unit's feet: to its right, ahead and up, in voxels.
	std::vector<float> right, ahead, up;
	/// Which part each belongs to.
	std::vector<Uint8> part;
	/// What the sprites show at each voxel: 16 palette colours a voxel, the 8 directions' frames painted
	/// level (as a picture taken at the unit's height, for a camera near its height) and then the same
	/// painted along the isometric view's slant (for a camera high above).
	std::vector<Uint8> colours;
	/// The walk: each of its 8 phases' places (the same voxels, legs and arms swung, the body bobbing).
	std::vector<float> walkRight[8], walkAhead[8], walkUp[8];
	/// A pose of the body going down: its standing body crumpled (its height times height, its breadth times spread),
	/// then tipped over by angle (degrees) towards fallToward (0 ahead, 2 its right, 4 back, 6 its left) about its edge
	/// on that side.
	struct FallPose { float height = 1.0f, spread = 1.0f, angle = 0.0f; };
	/// Going down as its death frames show it (the game's fall, drawn facing the screen): the pose each frame shows (none
	/// for a unit without death frames: it tips over backwards), the way it tips, and the colours each frame paints it
	/// in its pose (a voxel for each frame in turn; 0 where the frame doesn't show it: its standing colour).
	std::vector<FallPose> fall;
	int fallToward = 4;
	std::vector<Uint8> fallColours;
	/// How far the body reaches each way (0 ahead, clockwise by 45 degrees) from the middle of its feet, the items in its
	/// hands left out: the edge it tips over.
	float edge[8] = {};
	/// The pose a fall has come to (0 standing, 1 down), between its frames' poses.
	FallPose poseAt(double progress) const;
	/// Puts a body point (voxels from the middle of the feet: to the right, ahead, up) in a pose.
	void posed(const FallPose &pose, float &across, float &along, float &height) const;
	/// The fall's frame whose colours paint the body at this point of the fall, or -1 (its standing colours).
	int fallFrameAt(double progress) const;
	/// The colour of voxel k seen from direction (0-7) with level painting (or slanted).
	Uint8 colour(size_t k, int direction, bool level) const { return colours[k * 16 + (level ? 0 : 8) + direction]; }
	/// The views each voxel shows in, a bit for each of the 16 colours' views (level, then slanted: the bit of colour slot
	/// (level ? 0 : 8) + direction): the hand drawn frames don't quite agree on one shape, so seen from one of the 8
	/// directions the body shows only its voxels inside that direction's drawing, and pieces are added for what the
	/// drawing has that they leave bare (see matchViews in FirstPersonVoxelBodies.cpp): from each direction the body
	/// has its sprite's outline.
	std::vector<Uint16> views;
	/// The voxels from addedFrom on are such added pieces (left out of a fall: the body falls as its own shape).
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
	bool hitAt(float right, float ahead, float up, int direction, bool level) const;
};

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
	/// The body for a unit as it is now, made the first time from its sprites (drawn with the unit
	/// sprite given, which is left turned to the unit's own direction, its other parts drawn nowhere)
	/// its items' inventory pictures (BIGOBS.PCK) and its corpse's (FLOOROB.PCK), in the battle's palette, while builds (counted
	/// down: frames captured for a body) allows; null when there's none (yet: one being made; or
	/// walking before it was ever seen standing). walkPhase: out, the walk's phase to draw (or -1).
	const VoxelUnitModel *modelFor(BattleUnit *unit, UnitSprite &unitSprite, const SurfaceSet *inventorySprites, SurfaceSet *floorSprites,
		const SavedBattleGame *save, const SDL_Color *palette, int &builds, int &walkPhase);
	/// The body a unit was last drawn with (for its fall, and its corpse lying where it fell), or null.
	const VoxelUnitModel *lastBody(const BattleUnit *unit) const;
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
	/// Notes a unit's body as the one it was last drawn with; gives the body.
	const VoxelUnitModel *remember(const BattleUnit *unit, const std::shared_ptr<VoxelUnitModel> &body);
	/// The bodies being made (by what they're made from), and the keys waiting for one.
	std::set<Uint64> _making;
	std::map<Key, Uint64> _waiting;
	/// The builder's jobs, the bodies it has made, and which battle they're for (a new battle drops the old one's); and
	/// its thread (last: the rest is ready before it starts; it starts with the first job).
	std::mutex _mutex;
	std::condition_variable _wake;
	std::deque<std::unique_ptr<Job>> _jobs;
	std::vector<std::pair<Uint64, std::shared_ptr<VoxelUnitModel>>> _made;
	unsigned _battle = 0;
	const SavedBattleGame *_battleSave = nullptr; // (the battle the bodies are for, see forBattle)
	bool _stopping = false;
	std::thread _builder;
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
