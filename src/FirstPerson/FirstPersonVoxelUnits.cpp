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
 * FirstPersonVoxelUnits.cpp: units given a body (see FirstPersonVoxelUnits.h).
 *
 * The parts' frames come from the game's own unit drawer (UnitSprite), each part on a frame of its
 * own (setPartCapture) for each of the 8 directions, with the unit's colours as the game draws them;
 * its walk's and death's frames likewise, the drawer told which to draw (setWalkOverride,
 * setFallOverride: the unit itself left as it is).
 * The bodies are made from them (FirstPersonVoxelBodies.cpp) on a thread of their own.
 */
#include "FirstPersonVoxelUnits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // (std::min and std::max, not Windows' macros)
#endif
#include <windows.h>
#undef near
#undef far
#elif defined(__APPLE__)
#include <pthread.h>
#endif
#include "FirstPersonVoxelBodies.h"
#include "FirstPersonVoxelBodiesFine.h"
#include "FirstPersonVoxelModel.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Battlescape/UnitSprite.h"
#include "../Engine/GraphSubset.h"
#include "../Engine/Script.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleSoldier.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Soldier.h"

namespace OpenXcom
{

using namespace VoxelBodies;

namespace
{

/// The drawer's body parts the body's parts come from (see PartCount).
constexpr int PartSlots[PartCount] = { BODYPART_TORSO, BODYPART_LEFTARM, BODYPART_RIGHTARM, BODYPART_LEGS, BODYPART_ITEM_RIGHTHAND, BODYPART_ITEM_LEFTHAND };

/// FNV-1a: a hash of what a body is made from, so units drawn alike share one.
void hashBytes(Uint64 &hash, const Uint8 *bytes, size_t count)
{
	for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
}

/// A picture drawn as a dither counts as one from this share of holes among its pixels, in hundredths (see ditherHoles):
/// XPiratez's stealth and spectre suits have 69 to 87 holes for 100 pixels drawn, any other unit's sheet 9 at most.
const int DitherShare = 33;
/// A unit drawn the same from every side is lopsided, and made as a relief (see VoxelBodies::Routine), when fewer than
/// this many in a hundred of its drawing's pixels have their mirror pixel drawn too (see modelFor).
const int ReliefAlike = 75;
/// A unit that can't move is a mount (see modelFor) when this many of its eight drawings are the same picture.
const int MountAlike = 3;
/// A tank's turret tops out this many voxels up (see modelFor): the owner's number.
const int TankTop = 20;

/**
 * Counts a picture's drawn pixels and its dither holes: the empty pixels with drawn ones on three sides at least (in a
 * picture drawn as a dither, every other pixel left out so the map shows through, all those inside its outline).
 */
void ditherHoles(const std::vector<Uint8> &pixels, int width, int height, int &drawn, int &holes)
{
	if ((int)pixels.size() < width * height) return;
	auto filled = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && pixels[y * width + x] != 0; };
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (pixels[y * width + x]) ++drawn;
			else if (filled(x - 1, y) + filled(x + 1, y) + filled(x, y - 1) + filled(x, y + 1) >= 3) ++holes;
		}
	}
}

/**
 * Fills a dithered picture's holes (see ditherHoles), each with the colour beside it: a body can't be made of every
 * other pixel (the cells with too few neighbours go, and with them all of it but a scrap: a player, GitHub issue #185,
 * XPiratez's ghosts "don't seem to make a model at all").
 */
/// A frame's pixels, row by row, straight from its buffer (a body's making reads frames by the hundred, on the game's
/// own thread: read a pixel at a time, they were most of the time a body's frames took to gather); any: whether anything
/// is drawn on it.
std::vector<Uint8> pixelsOf(const Surface *surface, bool *any = nullptr)
{
	const int width = surface->getWidth(), height = surface->getHeight(), pitch = surface->getPitch();
	const Uint8 *buffer = surface->getBuffer();
	std::vector<Uint8> pixels((size_t)width * height);
	for (int row = 0; row < height; ++row) std::memcpy(&pixels[(size_t)row * width], buffer + (size_t)row * pitch, (size_t)width);
	if (any) *any = std::any_of(pixels.begin(), pixels.end(), [](Uint8 pixel) { return pixel != 0; });
	return pixels;
}

void fillDither(std::vector<Uint8> &pixels, int width, int height)
{
	if ((int)pixels.size() < width * height) return;
	const std::vector<Uint8> drawn = pixels;
	auto at = [&](int x, int y) -> Uint8 { return x >= 0 && y >= 0 && x < width && y < height ? drawn[y * width + x] : 0; };
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (drawn[y * width + x]) continue;
			const Uint8 beside[4] = { at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1) };
			if ((beside[0] != 0) + (beside[1] != 0) + (beside[2] != 0) + (beside[3] != 0) < 3) continue;
			pixels[y * width + x] = beside[0] ? beside[0] : beside[1];
		}
	}
}

}

/// What a body is made from, for the builder's thread: the unit's parts' frames, the items in its hands, its pose and
/// drawing routine, the battle's palette; and the hash of all that, the battle it's for.
struct VoxelUnits::Job
{
	PartFrames parts[PartCount];
	/// The parts as the game lays them over each other, for each direction (one-tile units: which part hides which).
	std::vector<Uint8> composite[8];
	HeldItem items[2];
	std::vector<std::vector<Uint8>> deaths;
	std::vector<Uint8> corpse;
	HeldItem lying; // (the corpse's inventory picture: the body lying)
	WalkFrames walks;
	bool aiming = false;
	bool still = false; // (it can't move: see Routine)
	float shows = 1.0f; // (how much of the body shows: see VoxelUnitModel::opacity)
	bool sparks = false; // (it hovers on sparks, by the unit table: see VoxelUnitModel::sparks)
	/// The unit's 8 standing drawings at each frame of the battle's animation (AnimFrames x 8, frame by frame), for a unit
	/// whose sprite changes shape with it (see modelFor); empty for any other.
	std::vector<std::vector<Uint8>> anim;
	bool beast = false; // (a long body on four legs, by the unit table: see Routine)
	float torsoDepth = 0.0f; // (the torso's slices at most this deep for their width, by the unit table: see Routine)
	bool backTop = false; // (what faces up painted by the back drawing, by the unit table: see Routine)
	bool frontal = false; // (kept to its front and back drawings' outlines, by the unit table: see Routine)
	bool relief = false; // (its one drawing given volume: see Routine)
	int arms = 0; // (its arms outside the body, coloured along their length, by the unit table: see Routine)
	bool standard = false; // (a person, made the standard humanoid, by the unit table: see Routine)
	bool sides = false; // (a big walker cut by its four drawings from front, back and sides, by the unit table: see Routine)
	int traits = 0; // (a round trunk, a head of face and hair, by the unit table: see Routine)
	float tankHull = 0.0f, tankTop = 0.0f; // (a tank's deck and top, voxels: see Routine)
	float flatTop = 1.0f, flatUnder = 1.0f; // (pressed flatter above and under its widest level, by the unit table: see Routine)
	bool fine = false; // (made twice as fine: see FineBodies)
	const Routine *routine = nullptr;
	int unitId = 0, status = 0; // (the unit's, for dump's file names)
	bool kneeled = false;
	SDL_Color palette[256];
	Uint64 hash = 0;
	unsigned battle = 0;
};

/**
 * Whether a unit can be drawn as a voxel body: of a size and drawn by a routine the bodies know (see Routine), and up
 * (not down or falling).
 * @param unit The unit.
 * @return True if it can.
 */
bool VoxelUnits::suits(const BattleUnit *unit)
{
	const Armor *armor = unit->getArmor();
	const Routine *routine = routineFor(armor->getDrawingRoutine());
	return routine && armor->getSize() == (routine->big ? 2 : 1) && !unit->isOut() && unit->getStatus() != STATUS_COLLAPSING;
}

/**
 * Whether a unit that is down (dead or unconscious: in a battle loaded with it down already, or one never seen up) can
 * have a body made all the same, as it stood, for its corpse to lie as (see FirstPersonMode::makeVoxelBodies): one
 * loaded down lies as one that went down in view does.
 * @param unit The unit.
 * @return True if it can.
 */
bool VoxelUnits::suitsDown(const BattleUnit *unit)
{
	const Armor *armor = unit->getArmor();
	const Routine *routine = routineFor(armor->getDrawingRoutine());
	return routine && armor->getSize() == (routine->big ? 2 : 1) && (unit->getStatus() == STATUS_DEAD || unit->getStatus() == STATUS_UNCONSCIOUS);
}

/**
 * Whether the game draws a unit otherwise while it aims: the routines of the bodies in parts (arms up on what they
 * hold), the one-frame bodies of routine 4 and its kin, and the Silacoid's (its own frame). The others (every unit two
 * tiles by two, the Chryssalid, the Celatid, the units drawn the same from every side) are drawn the same aiming or
 * not, and have one body for both: with aiming in their key each had a second body made at its first shot, alike in
 * every cube, and showed as its flat picture all the while that took (a player, GitHub issue #228: "2x2 units /
 * muckstars (units with no rotations) flash sprite in voxel mode when reaction firing").
 * @param routine The armour's drawing routine.
 * @return True if aiming shows.
 */
static bool aimingShows(int routine)
{
	switch (routine)
	{
	case 0: case 1: case 4: case 6: case 8: case 10: case 13: case 14: case 15: case 17: case 18: return true;
	default: return false;
	}
}

/**
 * The key a unit's body is kept under: the unit, its armour, kneeling, aiming (where that shows, see aimingShows),
 * flying, what it holds, and a big unit's turret's turn from its hull.
 * @param unit The unit.
 * @param standing As the unit would stand (the body the walk swings): not kneeling, aiming or flying.
 * @return The key.
 */
VoxelUnits::Key VoxelUnits::keyFor(const BattleUnit *unit, bool standing)
{
	auto held = [](const BattleItem *item) -> const RuleItem* {
		return item && (!item->getRules()->isFixed() || item->getRules()->getFixedShow()) ? item->getRules() : nullptr;
	};
	// (only a unit with a turret: the game moves every big unit's turret direction a step behind its own as it turns,
	// and a Cyberdisc turning or setting off had no body for that step: it showed as its picture)
	const int turret = unit->getArmor()->getSize() > 1 && unit->getTurretType() != -1 ? ((unit->getTurretDirection() - unit->getDirection()) % 8 + 8) % 8 : 0;
	return Key(unit->getId(), unit->getArmor(), !standing && unit->isKneeled(), !standing && unit->getStatus() == STATUS_AIMING && aimingShows(unit->getArmor()->getDrawingRoutine()),
		!standing && unit->isFloating(), held(unit->getRightHandWeapon()), held(unit->getLeftHandWeapon()), turret);
}

/**
 * The body for a unit as it is now, made from its sprites the first time it's seen so (a unit drawn just like one
 * seen before shares its body). A walking unit takes the body it stands in (and the walk's phase), if it was ever
 * seen standing; else there's none yet (it's drawn as its sprite meanwhile). While the body for how a unit is now
 * is still to be made or being made (it kneels, raises its weapon to aim, takes another in hand for the first time),
 * it keeps the body it was last drawn with: it showed as its flat picture for those frames, a flash at a soldier's
 * first shot (GitHub issue #228). One never drawn as a body is its sprite meanwhile.
 * @param unit The unit.
 * @param unitSprite The 3D view's unit drawer (left turned to the unit's own direction when a body is made; the parts
 *        it doesn't give the body, such as fire, are drawn on its surface, to be cleared).
 * @param inventorySprites The game's BIGOBS.PCK (the items in hand).
 * @param floorSprites The game's FLOOROB.PCK (the corpse's picture, as the body lies once down).
 * @param save The battle (the items' recolouring scripts).
 * @param palette The battle's palette (which colours are near black).
 * @param builds How many bodies may still be made (counted down when one is; at 0 a unit without a body gets none).
 * @param walkPhase Out: the walk's phase to draw (0-7), or -1 when not walking.
 * @return The body, or null.
 */
const VoxelUnitModel *VoxelUnits::modelFor(BattleUnit *unit, UnitSprite &unitSprite, const SurfaceSet *inventorySprites, SurfaceSet *floorSprites,
	const SavedBattleGame *save, const SDL_Color *palette, int &builds, int &walkPhase)
{
	walkPhase = -1;
	collect();
	if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
	{
		auto found = _models.find(keyFor(unit, true));
		if (found == _models.end() || !found->second) return nullptr;
		walkPhase = ((unit->getWalkingPhase() % 8) + 8) % 8;
		return remember(unit, found->second);
	}
	const Key key = keyFor(unit, false);
	auto found = _models.find(key);
	if (found != _models.end()) return remember(unit, found->second);
	auto waiting = _waiting.find(key);
	if (waiting != _waiting.end())
	{
		auto made = _byFrames.find(waiting->second);
		if (made == _byFrames.end()) return lastBody(unit); // (still being made)
		_models[key] = made->second;
		_waiting.erase(waiting);
		return remember(unit, made->second);
	}
	if (builds <= 0 || !palette) return lastBody(unit);
	// (Every unit whose frames are gathered counts, whether its body is then made or shared with one alike: gathering
	// them takes the game's own thread a millisecond or more. A hundred units alike, all gathered in one frame because
	// only the first was "made", held the game up for a third of a second at a battle's start, and again whenever
	// many came at once: a player, GitHub issue #204, "lag spikes".)
	--builds;

	const Routine *routine = routineFor(unit->getArmor()->getDrawingRoutine());
	if (!routine) return nullptr;
	unitSprite.setDrawOut(unit->isOut()); // (a unit already down, see suitsDown: drawn as it stood)
	// Each part on a frame of its own, for each direction (the drawer turned to it), with the game's colours; the
	// drawer's other parts on a frame nobody looks at. A big unit's four tiles (and its turret, and what it floats on) on
	// the torso's frame, laid out as the 3D view lays out its sprite.
	PartFrames parts[PartCount];
	std::vector<std::unique_ptr<Surface>> frames;
	Surface discard(FrameSize, FrameSize);
	Surface *capture[BODYPART_LARGE_TURRET + 4];
	std::fill(std::begin(capture), std::end(capture), &discard);
	for (int p = 0; p < PartCount; ++p)
	{
		frames.push_back(std::make_unique<Surface>(FrameSize, FrameSize));
		capture[PartSlots[p]] = frames.back().get();
	}
	if (routine->big)
	{
		for (int slot = BODYPART_LARGE_TORSO; slot < BODYPART_LARGE_TURRET + 4; ++slot) capture[slot] = frames[0].get();
		// (A tank's turret on a frame of its own, the left arm's: the hull and the turret are made a body each, the
		// turret standing on the hull, see makeModel. On one frame with the hull, the two came out one low cone: a
		// player, GitHub issue #188, the tank "looks almost like a dopey pyramid with no protruding barrel".)
		// (Not a unit that can't move: a turret on a vehicle is one plain block, all of it: see makeModel.)
		if (unit->getTurretType() != -1 && unit->getArmor()->allowsMoving()) capture[BODYPART_LARGE_TURRET] = frames[1].get();
	}
	const GraphSubset mask(FrameSize, FrameSize);
	// (and, for a unit in parts, all its parts on one frame, laid over each other as the game draws them)
	std::vector<Uint8> composite[8];
	Surface layered(FrameSize, FrameSize);
	Surface *together[BODYPART_LARGE_TURRET + 4];
	std::fill(std::begin(together), std::end(together), &discard);
	for (int p = 0; p < PartCount; ++p) together[PartSlots[p]] = &layered;
	// (All of it at the first frame of the battle's animation, whenever the unit is first seen: two units alike are
	// drawn alike then, and share a body; one whose colours the animation changes had a body for whichever frame it
	// was first seen at, each made anew.)
	// (The first frame at which its mod's recolouring of its sprite is at rest, see restFrame: XPiratez's and The X-Com
	// Files' energy shields flash a unit's sprite blue for 4 frames in every 32, each unit at its own time, and a body
	// made in those frames was blue for good.)
	const int animNow = save->getAnimFrame();
	unitSprite.setAnimationFrame(restFrame(unit, save));
	auto drawFacing = [&](int d) {
		for (auto &frame : frames) frame->clear();
		unitSprite.setViewRotation(d - unit->getDirection());
		unitSprite.setPartCapture(capture);
		if (routine->big)
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
			unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
			unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
		}
		else
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
		}
		unitSprite.setPartCapture(nullptr);
	};
	// A quick look first: its front and back drawings, with what it is and how it stands. A unit just like one whose
	// frames were gathered before (the same armour and kind, pose and items, and the same two drawings: a body's colours
	// are those two's) takes that one's body, without the hundred drawings a body's frames are. (A battle of a hundred
	// units alike gathered them all, three milliseconds each on the game's own thread: a player, GitHub issue #204.)
	Uint64 quick = 14695981039346656037ULL;
	try
	{
		for (int d : { Front, (Front + 4) % 8 })
		{
			drawFacing(d);
			for (auto &frame : frames)
				for (int row = 0; row < FrameSize; ++row) hashBytes(quick, frame->getBuffer() + (size_t)row * frame->getPitch(), FrameSize);
		}
	}
	catch (const Exception &)
	{
		// (a sheet short of the frames its routine draws: no body, as its sprite; asked again it would throw again)
		unitSprite.setPartCapture(nullptr);
		unitSprite.setViewRotation(0);
		unitSprite.setDrawOut(false);
		unitSprite.setAnimationFrame(animNow);
		_models[key] = nullptr;
		return nullptr;
	}
	{
		const Uint64 said[4] = { (Uint64)(size_t)unit->getArmor(), (Uint64)std::hash<std::string>()(unit->getType()), (Uint64)(size_t)std::get<5>(key), (Uint64)(size_t)std::get<6>(key) };
		hashBytes(quick, (const Uint8*)said, sizeof(said));
		const Uint8 stands[6] = { (Uint8)unit->isKneeled(), (Uint8)std::get<3>(key), (Uint8)unit->isFloating(), (Uint8)std::get<7>(key), (Uint8)FineBodies, (Uint8)unit->isOut() };
		hashBytes(quick, stands, sizeof(stands));
	}
	auto quickKnown = _byQuick.find(quick);
	if (quickKnown != _byQuick.end())
	{
		unitSprite.setViewRotation(0);
		unitSprite.setDrawOut(false);
		unitSprite.setAnimationFrame(animNow);
		auto alike = _byFrames.find(quickKnown->second);
		if (alike != _byFrames.end())
		{
			_models[key] = alike->second;
			return remember(unit, alike->second);
		}
		_waiting[key] = quickKnown->second; // (being made)
		return lastBody(unit);
	}
	for (int d = 0; d < 8; ++d)
	{
		drawFacing(d);
		for (int p = 0; p < PartCount; ++p)
		{
			bool any = false;
			parts[p].frame[d] = pixelsOf(frames[p].get(), &any);
			parts[p].any |= any;
		}
		if (!routine->big && !routine->whole)
		{
			layered.clear();
			unitSprite.setPartCapture(together);
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.setPartCapture(nullptr);
			composite[d] = pixelsOf(&layered);
		}
	}
	// The death frames (the fall as the game draws it, facing the screen), for the body to fall as they show.
	std::vector<std::vector<Uint8>> deaths;
	if (!routine->big)
	{
		Surface *fallCapture[BODYPART_LARGE_TURRET + 4];
		std::fill(std::begin(fallCapture), std::end(fallCapture), &discard);
		fallCapture[BODYPART_COLLAPSING] = frames[0].get();
		unitSprite.setViewRotation(Front - unit->getDirection());
		for (int frame = 0; frame < unit->getArmor()->getDeathFrames() && frame < 32; ++frame)
		{
			frames[0]->clear();
			unitSprite.setFallOverride(frame);
			unitSprite.setPartCapture(fallCapture);
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.setPartCapture(nullptr);
			unitSprite.setFallOverride(-1);
			bool any = false;
			std::vector<Uint8> pixels = pixelsOf(frames[0].get(), &any);
			if (!any) break;
			deaths.push_back(std::move(pixels));
		}
	}
	// The walk's frames, each phase from the right side and the front right (see WalkFrames), for the walk to follow them:
	// its legs' (all of it, for a body drawn on one frame) and its arms'.
	WalkFrames walks;
	// (Not of a unit that can't move: a turret drawn by a walker's routine has no walk frames in its sheet, and the game's
	// drawer throws when asked for a missing frame. XPiratez's truck turret took the game down when its body was made. A
	// sheet short of frames for a unit that may move: no walk frames either, the body stands.)
	if ((routine->legsSwing || routine->armsSwing) && unit->getArmor()->allowsMoving())
	try
	{
		for (int phase = 0; phase < 8; ++phase)
		{
			for (int view = 0; view < 2; ++view)
			{
				for (auto &frame : frames) frame->clear();
				unitSprite.setViewRotation(WalkViews[view] - unit->getDirection());
				unitSprite.setWalkOverride(phase);
				unitSprite.setPartCapture(capture);
				if (routine->big)
				{
					unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
					unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
					unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
					unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
				}
				else
				{
					unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
				}
				unitSprite.setPartCapture(nullptr);
				unitSprite.setWalkOverride(-1);
				walks.legs[phase][view] = pixelsOf(frames[routine->whole ? 0 : 3].get());
				if (!routine->whole)
				{
					walks.arms[phase][view][0] = pixelsOf(frames[2].get());
					walks.arms[phase][view][1] = pixelsOf(frames[1].get());
				}
			}
		}
		walks.any = true;
	}
	catch (const Exception &)
	{
		unitSprite.setPartCapture(nullptr);
		unitSprite.setWalkOverride(-1);
		walks = WalkFrames();
	}
	// What the mod's rules say of the unit by its names (the unit table: a ghost seen through, a beast on four legs).
	const Unit *unitRules = unit->getUnitRules();
	const Soldier *soldier = unit->getGeoscapeSoldier();
	// (and by the routine the game draws it with, as the name "routine:N": the big mods have thousands of units, most of
	// them people drawn as the games' own soldiers are, under every kind of name)
	const RuleFirstPerson::UnitLook look = save->getMod()->getFirstPerson().getUnitLook({ unit->getType(), unit->getArmor()->getType(),
		unit->getArmor()->getSpriteSheet(), unitRules ? unitRules->getRace() : std::string(), soldier ? soldier->getRules()->getType() : std::string(),
		"routine:" + std::to_string(unit->getArmor()->getDrawingRoutine()) });
	// A unit whose standing drawing changes shape with the battle's animation frame (the game's routine draws a frame
	// of it for each: the Silacoid and the Celatid pulse; or a mod's script picks its sprite by it: X-Com Files'
	// shadowbat flaps as it hovers) has a body for each frame, as its sprite has a drawing (the owner: "voxel models
	// should generally follow the sprites"; a player, GitHub issue #195: the bat "looks exactly like that and never
	// flaps"; #210: "other animated units also dont move"). Any unit drawn on one frame, one tile big, with nothing
	// showing in its hands (a built-in weapon the game doesn't draw is nothing: the bat's bite kept it from flapping in
	// a real battle), unless the unit table says no (animation: none): the front drawing at the 7 frames after this one
	// differs in outline, and is the same again at the 8th. (A change of colours alone, a shield's flash, a hit's
	// blink, is not an animation.)
	auto shownInHand = [](const BattleItem *item) { return item && (!item->getRules()->isFixed() || item->getRules()->getFixedShow()); };
	std::vector<std::vector<Uint8>> anim;
	if (look.animated != 0 && routine->whole && !routine->big && !shownInHand(unit->getRightHandWeapon()) && !shownInHand(unit->getLeftHandWeapon()))
	{
		auto drawn = [&](int animFrame, int d) {
			frames[0]->clear();
			unitSprite.setAnimationFrame(animFrame);
			unitSprite.setViewRotation(d - unit->getDirection());
			unitSprite.setPartCapture(capture);
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.setPartCapture(nullptr);
			return pixelsOf(frames[0].get());
		};
		auto sameOutline = [](const std::vector<Uint8> &one, const std::vector<Uint8> &other) {
			for (size_t i = 0; i < one.size(); ++i) if ((one[i] != 0) != (other[i] != 0)) return false;
			return true;
		};
		try
		{
			const std::vector<Uint8> first = drawn(0, Front);
			bool moves = false;
			for (int later = 1; later < AnimFrames && !moves; ++later) moves = !sameOutline(first, drawn(later, Front));
			if (moves && sameOutline(first, drawn(AnimFrames, Front)))
			{
				// (from frame 0 of a round: the bodies are kept by the frame's number)
				for (int frame = 0; frame < AnimFrames; ++frame)
					for (int d = 0; d < 8; ++d) anim.push_back(drawn(frame, d));
			}
		}
		catch (const Exception &)
		{
			unitSprite.setPartCapture(nullptr);
			anim.clear();
		}
		unitSprite.setAnimationFrame(0);
	}
	// (the body itself is the first frame's then: units alike share their bodies whatever frame each was first seen at)
	for (int d = 0; d < 8 && anim.size() >= 8; ++d) parts[0].frame[d] = anim[d];
	// A unit drawn the same from every side (the game's Silacoid, Celatid and Bio-Drone routines draw one picture
	// whatever way the unit faces) and lopsided: its body is that one drawing given volume (see Routine::relief). Carved
	// from eight drawings all the same, a body keeps only what the drawing has both sides of its middle: enough of a
	// Celatid or a Silacoid, which stay carved as they were; of X-Com Files' Abomination, bubbles off to one side,
	// "flat looking puddles" (a player, GitHub issue #203). Lopsided: under ReliefAlike of the drawing's pixels have
	// their mirror pixel drawn too. The unit table's shape says otherwise for the units it names (relief, upright).
	bool relief = false;
	if (routine->whole && !routine->big && look.relief != 0)
	{
		bool allRound = true;
		for (int d = 1; d < 8 && allRound; ++d) allRound = parts[0].frame[d] == parts[0].frame[0];
		if (allRound)
		{
			int drawn = 0, mirrored = 0;
			const std::vector<Uint8> &front = parts[0].frame[Front];
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column)
				{
					if (!front[row * FrameSize + column]) continue;
					++drawn;
					if (front[row * FrameSize + FrameSize - 1 - column]) ++mirrored;
				}
			relief = look.relief == 1 || (drawn > 0 && mirrored * 100 < drawn * ReliefAlike);
		}
	}
	unitSprite.setViewRotation(0);
	unitSprite.setDrawOut(false);
	unitSprite.setAnimationFrame(animNow);
	// A mount: a unit that can't move, one tile big, drawn the same from MountAlike of its eight sides or more, though
	// not from all: a gun on a plate bolted to a vehicle's side, the plate drawn the same whichever way the unit
	// faces, the gun only where it can point (the 40k mod's sponsons). Its drawings are no body's seen from eight
	// sides, and what the eight leave in common is a crumb: it has no body, and is drawn as its picture, as with the
	// setting off ("turret SEEMS to have issue displaying because its a unit limited in turn radius", a player, GitHub
	// issue #233). (A body that turns is drawn differently from every side; one drawn the same from all eight is a
	// Silacoid's or a relief, see above.)
	if (routine->whole && !routine->big && !relief && !unit->getArmor()->allowsMoving())
	{
		int most = 0;
		for (int d = 0; d < 8; ++d)
		{
			int alike = 0;
			for (int other = 0; other < 8; ++other) alike += parts[0].frame[other] == parts[0].frame[d];
			most = std::max(most, alike);
		}
		if (most >= MountAlike && most < 8)
		{
			_models[key] = nullptr;
			return nullptr;
		}
	}
	// And its corpse's picture on the floor (FLOOROB.PCK, as the map draws a corpse), for the body to end lying as it shows.
	std::vector<Uint8> corpse;
	const auto &corpseItems = unit->getArmor()->getCorpseBattlescape();
	// (A big unit's: its four parts' pictures where the map draws them, as its own four tiles are laid out: the wreck whole.)
	const bool wreck = routine->big && floorSprites && corpseItems.size() >= 4;
	if (floorSprites && !corpseItems.empty() && corpseItems.front() && (!routine->big || wreck))
	{
		Surface *picture = floorSprites->getFrame(corpseItems.front()->getFloorSprite());
		if (picture)
		{
			frames[0]->clear();
			if (wreck)
			{
				for (int part = 0; part < 4; ++part)
				{
					Surface *piece = corpseItems[part] ? floorSprites->getFrame(corpseItems[part]->getFloorSprite()) : nullptr;
					if (piece) piece->blitNShade(frames[0].get(), 16 + (part % 2 - part / 2) * 16, 8 + (part % 2 + part / 2) * 8, 0);
				}
			}
			else picture->blitNShade(frames[0].get(), 16, 8, 0);
			bool any = false;
			corpse = pixelsOf(frames[0].get(), &any);
			if (!any) corpse.clear();
		}
	}
	// And its corpse's picture in the inventory (BIGOBS.PCK): laid on the floor and given volume, the body lying.
	HeldItem lying;
	if (!routine->big && inventorySprites && !corpseItems.empty() && corpseItems.front())
	{
		if (const Surface *picture = inventorySprites->getFrame(corpseItems.front()->getBigSprite()))
		{
			lying.width = picture->getWidth();
			lying.height = picture->getHeight();
			lying.picture.resize(lying.width * lying.height);
			bool any = false;
			for (int y = 0; y < lying.height; ++y)
				for (int x = 0; x < lying.width; ++x) any |= (lying.picture[y * lying.width + x] = picture->getPixel(x, y)) != 0;
			if (!any) lying = HeldItem();
		}
	}

	// A unit drawn as a dither (XPiratez's stealth and spectre suits, see-through on the map) has its pictures' holes
	// filled, for a body whole as any other's (see fillDither); so its corpse's pictures, if they are dithered.
	bool dithered = false;
	{
		int drawn = 0, holes = 0;
		for (const PartFrames &part : parts)
			for (const auto &frame : part.frame) ditherHoles(frame, FrameSize, FrameSize, drawn, holes);
		if (drawn > 0 && holes * 100 >= drawn * DitherShare)
		{
			dithered = true;
			for (PartFrames &part : parts)
				for (auto &frame : part.frame) fillDither(frame, FrameSize, FrameSize);
			for (auto &frame : composite) fillDither(frame, FrameSize, FrameSize);
			for (auto &death : deaths) fillDither(death, FrameSize, FrameSize);
			for (auto &frame : anim) fillDither(frame, FrameSize, FrameSize);
			for (auto &phase : walks.legs)
				for (auto &view : phase) fillDither(view, FrameSize, FrameSize);
			for (auto &phase : walks.arms)
				for (auto &view : phase)
					for (auto &side : view) fillDither(side, FrameSize, FrameSize);
		}
		drawn = holes = 0;
		ditherHoles(corpse, FrameSize, FrameSize, drawn, holes);
		if (drawn > 0 && holes * 100 >= drawn * DitherShare) fillDither(corpse, FrameSize, FrameSize);
		drawn = holes = 0;
		ditherHoles(lying.picture, lying.width, lying.height, drawn, holes);
		if (drawn > 0 && holes * 100 >= drawn * DitherShare) fillDither(lying.picture, lying.width, lying.height);
	}

	// The items in hand: their inventory pictures as the game draws them in a hand (with their own recolouring).
	HeldItem items[2];
	const BattleItem *hands[2] = { unit->getRightHandWeapon(), unit->getLeftHandWeapon() };
	for (int hand = 0; hand < 2; ++hand)
	{
		const BattleItem *item = hands[hand];
		if (!item || (item->getRules()->isFixed() && !item->getRules()->getFixedShow())) continue;
		items[hand].twoHanded = item->getRules()->isTwoHanded();
		items[hand].melee = item->getRules()->getBattleType() == BT_MELEE;
		const Surface *frame = inventorySprites ? item->getBigSprite(inventorySprites, save, 0) : nullptr;
		if (!frame) continue;
		if (items[hand].melee) items[hand].share = handShare(item->getRules(), save->getMod(), inventorySprites);
		Surface picture(frame->getWidth(), frame->getHeight());
		ScriptWorkerBlit worker;
		BattleItem::ScriptFill(&worker, item, save, BODYPART_ITEM_INVENTORY, 0, 0);
		worker.executeBlit(frame, &picture, 0, 0, 0);
		clearSlotFrame(&picture, handSlotBottom(item->getRules()->getInventoryHeight())); // (a mod's frame drawn for the inventory screen)
		items[hand].width = picture.getWidth();
		items[hand].height = picture.getHeight();
		items[hand].picture.resize(items[hand].width * items[hand].height);
		for (int y = 0; y < items[hand].height; ++y)
			for (int x = 0; x < items[hand].width; ++x) items[hand].picture[y * items[hand].width + x] = picture.getPixel(x, y);
	}

	// A unit drawn just like one before (the same frames, pictures and pose) shares its body.
	// (aiming only tells how what it holds is held: a unit showing nothing in its hands, drawn the same aiming or
	// not, has one body for both, not a second made at its first shot)
	const bool aiming = std::get<3>(key) && (!items[0].picture.empty() || !items[1].picture.empty() || !routine->whole);
	Uint64 hash = 14695981039346656037ULL;
	for (const PartFrames &part : parts)
		for (const auto &frame : part.frame) hashBytes(hash, frame.data(), frame.size());
	for (const HeldItem &held : items)
	{
		hashBytes(hash, held.picture.data(), held.picture.size());
		const Uint8 flags[5] = { (Uint8)held.width, (Uint8)held.height, (Uint8)held.twoHanded, (Uint8)held.melee, (Uint8)std::lround(held.share * 100.0f) };
		hashBytes(hash, flags, 5);
	}
	for (const auto &death : deaths) hashBytes(hash, death.data(), death.size());
	hashBytes(hash, corpse.data(), corpse.size());
	hashBytes(hash, lying.picture.data(), lying.picture.size());
	for (int phase = 0; phase < 8 && walks.any; ++phase)
		for (int view = 0; view < 2; ++view)
		{
			hashBytes(hash, walks.legs[phase][view].data(), walks.legs[phase][view].size());
			for (int side = 0; side < 2; ++side) hashBytes(hash, walks.arms[phase][view][side].data(), walks.arms[phase][view][side].size());
		}
	float shows = dithered ? VoxelDitherFaint : 1.0f;
	if (look.shows >= 0) shows = std::min(shows, look.shows / 100.0f);
	for (const auto &frame : anim) hashBytes(hash, frame.data(), frame.size());
	// A tank (a big unit with a turret of its own that moves: see makeModel) stands about as high as the game's units
	// do: its deck at a kneeling soldier's height, by the rules of the game's first kind of soldier (14 in the games
	// themselves), its turret's top at TankTop, 20 voxels (a standing soldier is 22: the owner, "make tank only 20
	// voxels tall"). See VoxelBodies::Routine::tankHull.
	float tankHull = 0.0f, tankTop = 0.0f;
	if (routine->big && unit->getTurretType() != -1 && unit->getArmor()->allowsMoving())
	{
		tankHull = 14.0f;
		tankTop = (float)TankTop;
		const std::vector<std::string> &soldiers = save->getMod()->getSoldiersList();
		const RuleSoldier *normal = soldiers.empty() ? nullptr : save->getMod()->getSoldier(soldiers.front());
		if (normal && normal->getKneelHeight() > 0) tankHull = (float)normal->getKneelHeight();
		tankHull = std::min(tankHull, tankTop - 4.0f); // (room for the turret)
	}
	const Uint8 pose[12] = { (Uint8)aiming, (Uint8)routine->number, (Uint8)FineBodies, (Uint8)std::lround(shows * 100.0f), (Uint8)(look.beast == 1),
		(Uint8)std::max(look.torsoDepth, 0), (Uint8)(look.backTop == 1), (Uint8)std::max(look.frontal, 0), (Uint8)relief, (Uint8)std::max(look.arms, 0), (Uint8)(look.standard == 1), (Uint8)((look.roundTrunk == 1 ? 1 : 0) | (look.faceHead == 1 ? 2 : 0) | (look.tail == 1 ? 4 : 0) | (look.hullPaint == 1 ? 8 : 0) | (look.legs > 0 ? (look.legs / 2) << 4 : 0)) };
	hashBytes(hash, pose, 12);
	const Uint8 tankHeights[2] = { (Uint8)tankHull, (Uint8)tankTop };
	hashBytes(hash, tankHeights, 2);
	// (pressed flatter, by the unit table: only said of a unit hashed in, so every other body's hash is as it was)
	const int flatTop = look.flatTop > 0 ? look.flatTop : 100, flatUnder = look.flatUnder > 0 ? look.flatUnder : 100;
	if (flatTop != 100 || flatUnder != 100)
	{
		const Uint8 flats[2] = { (Uint8)flatTop, (Uint8)flatUnder };
		hashBytes(hash, flats, 2);
	}
	// (a turret's barrels, by the unit table: only said of a unit hashed in, as above)
	if (look.barrels >= 0)
	{
		const Uint8 barrels[2] = { 'b', (Uint8)look.barrels };
		hashBytes(hash, barrels, 2);
	}
	// (hovering on sparks, by the unit table: only said of a unit hashed in, as above; units alike share a body)
	if (look.sparks == 1)
	{
		const Uint8 hover[2] = { 's', 1 };
		hashBytes(hash, hover, 2);
	}
	// (kneeling, sat low, by the unit table: only a kneeling body it is said of is hashed in, as above)
	if (look.kneelLow == 1 && unit->isKneeled())
	{
		const Uint8 kneel[2] = { 'k', 1 };
		hashBytes(hash, kneel, 2);
	}
	_byQuick[quick] = hash;
	auto alike = _byFrames.find(hash);
	if (alike != _byFrames.end())
	{
		_models[key] = alike->second;
		return remember(unit, alike->second);
	}
	// Else the builder makes it (unless it's making one alike already), and the unit is drawn meanwhile as the body
	// it was last drawn with (see modelFor), or as its sprite if it never had one.
	_waiting[key] = hash;
	if (_making.insert(hash).second)
	{
		auto job = std::make_unique<Job>();
		std::move(std::begin(parts), std::end(parts), std::begin(job->parts));
		std::move(std::begin(composite), std::end(composite), std::begin(job->composite));
		std::move(std::begin(items), std::end(items), std::begin(job->items));
		job->deaths = std::move(deaths);
		job->corpse = std::move(corpse);
		job->lying = std::move(lying);
		job->walks = std::move(walks);
		job->aiming = aiming;
		job->still = !unit->getArmor()->allowsMoving();
		job->shows = shows;
		job->sparks = look.sparks == 1;
		job->beast = look.beast == 1;
		job->anim = std::move(anim);
		job->torsoDepth = look.torsoDepth > 0 ? look.torsoDepth / 100.0f : 0.0f;
		job->backTop = look.backTop == 1;
		job->frontal = look.frontal == 1;
		job->relief = relief;
		job->arms = std::max(look.arms, 0);
		job->standard = look.standard == 1;
		job->sides = look.frontal == 2;
		job->traits = (look.roundTrunk == 1 ? 1 : 0) | (look.faceHead == 1 ? 2 : 0) | (look.tail == 1 ? 4 : 0) | (look.hullPaint == 1 ? 8 : 0) | (look.legs > 0 ? (look.legs / 2) << 4 : 0);
		// (and a turret's barrels by the unit table, one more than the count: 0 nothing said; a tank's turret's turn
		// from its hull: see VoxelBodies::makeModel)
		job->traits |= (look.barrels >= 0 ? (std::min(look.barrels, 4) + 1) << 8 : 0) | (std::get<7>(key) & 7) << 11;
		// (kneeling, sat low, by the unit table: see sitLow in FirstPersonVoxelBodies.cpp)
		if (look.kneelLow == 1 && unit->isKneeled()) job->traits |= 128;
		job->tankHull = tankHull;
		job->tankTop = tankTop;
		job->flatTop = flatTop / 100.0f;
		job->flatUnder = flatUnder / 100.0f;
		job->fine = FineBodies;
		job->routine = routine;
		std::copy(palette, palette + 256, job->palette);
		job->hash = hash;
		job->unitId = unit->getId();
		job->status = unit->getStatus();
		job->kneeled = unit->isKneeled();
		// (a builder's thread more for each body waiting, up to BuilderThreads: a squad's bodies are made side by side)
		std::lock_guard<std::mutex> lock(_mutex);
		job->battle = _battle;
		_jobs.push_back(std::move(job));
		if (_builders.size() < std::min(_jobs.size() + _building, builderThreads())) _builders.emplace_back(&VoxelUnits::build, this);
		_wake.notify_one();
	}
	return lastBody(unit);
}

/**
 * Notes a unit's body as the one it was last drawn with (a null body, of frames that can't make one, isn't).
 * @param unit The unit.
 * @param body Its body.
 * @return The body.
 */
const VoxelUnitModel *VoxelUnits::remember(const BattleUnit *unit, const std::shared_ptr<VoxelUnitModel> &body)
{
	if (body)
	{
		_lastBodies[unit->getId()] = body;
		if (!unit->isOutThresholdExceed()) _lastFacings[unit->getId()] = unit->getDirection(); // (the game turns one going down to face the screen)
	}
	return body.get();
}

/**
 * The way a unit faced when it was last drawn as a body, up (see lastBody).
 * @param unit The unit.
 * @return The direction (0-7); its own now if it was never drawn as one.
 */
int VoxelUnits::lastFacing(const BattleUnit *unit) const
{
	auto found = _lastFacings.find(unit->getId());
	return found != _lastFacings.end() ? found->second : unit->getDirection();
}

/**
 * The body a unit was last drawn with: what falls when it goes down, and lies where it fell.
 * @param unit The unit.
 * @return The body, or null if it was never drawn as one.
 */
/**
 * How long a melee weapon is for its hand drawing: the mods draw several in the hand with one drawing (XPiratez its
 * shiv and its machete), and a body's item is made as long as that drawing has it (see VoxelBodies::placeItem). So an
 * inventory picture's own length counts: the picture's (its drawn box, corner to corner) over the longest among the
 * mod's melee weapons with the same hand drawing. 1 for a weapon whose hand drawing is its own, as the games' all are.
 * Found once for each hand drawing and kept.
 * @param rule The weapon's rules.
 * @param mod The mod.
 * @param inventorySprites The inventory pictures (BIGOBS.PCK).
 * @return Its share of the hand drawing's length: over 0, 1 at most.
 */
float VoxelUnits::handShare(const RuleItem *rule, const Mod *mod, const SurfaceSet *inventorySprites)
{
	auto reach = [&](const RuleItem *item) -> float {
		const Surface *frame = item->getBigSprite() >= 0 ? const_cast<SurfaceSet*>(inventorySprites)->getFrame(item->getBigSprite()) : nullptr;
		if (!frame) return 0.0f;
		std::vector<Uint8> screens; // (what a mod drew for the inventory screen is no part of the item: see slotFrame)
		slotFrame(frame, handSlotBottom(item->getInventoryHeight()), screens);
		int left = frame->getWidth(), right = -1, top = frame->getHeight(), bottom = -1;
		for (int y = 0; y < frame->getHeight(); ++y)
			for (int x = 0; x < frame->getWidth(); ++x)
			{
				if (!frame->getPixel(x, y) || screens[y * frame->getWidth() + x]) continue;
				left = std::min(left, x); right = std::max(right, x);
				top = std::min(top, y); bottom = std::max(bottom, y);
			}
		// (not hypot on floats: the release's old C library has none)
		const float wide = (float)(right - left + 1), high = (float)(bottom - top + 1);
		return right < 0 ? 0.0f : std::sqrt(wide * wide + high * high);
	};
	if (rule->getHandSprite() < 0) return 1.0f;
	auto known = _handLongest.find(rule->getHandSprite());
	if (known == _handLongest.end())
	{
		float longest = 0.0f;
		for (const std::string &name : mod->getItemsList())
		{
			const RuleItem *other = mod->getItem(name);
			if (other && other->getBattleType() == BT_MELEE && other->getHandSprite() == rule->getHandSprite()) longest = std::max(longest, reach(other));
		}
		known = _handLongest.emplace(rule->getHandSprite(), longest).first;
	}
	const float own = reach(rule);
	return own > 0.0f && known->second > own ? std::max(HandShareLeast, own / known->second) : 1.0f;
}

bool VoxelUnits::recolourTable(const BattleUnit *unit, const SavedBattleGame *save, int frame, Uint8 table[256])
{
	if (!unit->getArmor()->getScript<ModScript::RecolorUnitSprite>()) return false;
	// (a strip of every colour drawn as the unit's torso is: the game's own way through the script, see UnitSprite::blitBody)
	static thread_local Surface strip(256, 1), drawn(256, 1);
	strip.lock(); drawn.lock();
	for (int colour = 0; colour < 256; ++colour) { *strip.getRaw(colour, 0) = (Uint8)colour; *drawn.getRaw(colour, 0) = 0; }
	ScriptWorkerBlit work;
	BattleUnit::ScriptFill(&work, unit, save, BODYPART_TORSO, frame, 0, 0);
	work.executeBlit(&strip, &drawn, 0, 0, 0);
	for (int colour = 0; colour < 256; ++colour) table[colour] = *drawn.getRaw(colour, 0) ? *drawn.getRaw(colour, 0) : (Uint8)colour;
	strip.unlock(); drawn.unlock();
	return true;
}

int VoxelUnits::restFrame(const BattleUnit *unit, const SavedBattleGame *save)
{
	Uint8 tables[48][256];
	auto known = _restFrames.find(unit->getId());
	// (The frame found before, while the sprite is still recoloured there as it was. A flash is told by the frame of
	// the hit that set it off: XPiratez's hit effect is on at every frame up to three past the unit's last hit, every
	// earlier frame too. A frame at rest when it was found is in the flash of any later hit.)
	if (known != _restFrames.end()
		&& (!known->second.scripted || (recolourTable(unit, save, known->second.frame, tables[0]) && std::equal(tables[0], tables[0] + 256, known->second.table))))
	{
		return known->second.frame;
	}
	// At rest: as it is 8 and 16 frames on too (a flash is a few frames long; a recolouring that never changes is at
	// rest at once). Looked for from now on, over 32 frames: a body made of a unit hit earlier in the battle, in a
	// battle loaded above all, was looked for at rest from the battle's first frames on, all of them in its hit's
	// flash, and stood in the flash's one colour for good, its corpse after it (a player, GitHub issue #241, "smoke
	// effect seems to trigger improperly ... this was after a load"). None: now, as ever.
	RestFrame &rest = _restFrames[unit->getId()];
	const int now = save->getAnimFrame();
	rest.frame = 0;
	rest.scripted = true;
	for (int ahead = 0; rest.scripted && ahead < 48; ++ahead) rest.scripted = recolourTable(unit, save, now + ahead, tables[ahead]);
	if (!rest.scripted) return rest.frame; // (no script: the battle's first frame, so that two units alike share a body)
	int found = 0;
	for (int ahead = 0; ahead < 32; ++ahead)
	{
		if (std::equal(tables[ahead], tables[ahead] + 256, tables[ahead + 8]) && std::equal(tables[ahead], tables[ahead] + 256, tables[ahead + 16])) { found = ahead; break; }
	}
	rest.frame = now + found;
	std::copy(tables[found], tables[found] + 256, rest.table);
	// (One of the battle's first frames instead, where the sprite is drawn there as at rest: two units alike are then
	// made at one frame, as those without a script are, and share a body.)
	for (int first = 0; first < 32; ++first)
	{
		if (recolourTable(unit, save, first, tables[0]) && std::equal(tables[0], tables[0] + 256, rest.table)) { rest.frame = first; break; }
	}
	return rest.frame;
}

const VoxelUnitModel *VoxelUnits::lastBody(const BattleUnit *unit) const
{
	auto found = _lastBodies.find(unit->getId());
	return found != _lastBodies.end() ? found->second.get() : nullptr;
}

/**
 * The body for a unit as it is now, if it's made (for what a shot meets: see FirstPerson::spriteHit); nothing is made
 * here (see modelFor). A unit walking or flying: the body it stands in.
 * @param unit The unit.
 * @return The body, or null.
 */
const VoxelUnitModel *VoxelUnits::bodyNow(const BattleUnit *unit)
{
	collect();
	const Key key = keyFor(unit, unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING);
	auto found = _models.find(key);
	if (found != _models.end()) return found->second.get();
	auto waiting = _waiting.find(key);
	if (waiting == _waiting.end()) return nullptr;
	auto made = _byFrames.find(waiting->second);
	return made != _byFrames.end() ? made->second.get() : nullptr;
}

/**
 * Whether the builder has no body left to make of those asked for so far: none waiting, none being made (those made
 * are taken at the next modelFor or bodyNow).
 * @return True if so.
 */
bool VoxelUnits::idle()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _jobs.empty() && _building == 0;
}

/**
 * The bodies are for this battle (each 3D view says so as it gets its battle): every body is forgotten if they were
 * another battle's; views of the same battle share them.
 * @param save The battle.
 */
void VoxelUnits::forBattle(const SavedBattleGame *save)
{
	if (save != _battleSave)
	{
		clear();
		_battleSave = save;
	}
}

/**
 * Nothing made yet: the builder's thread starts with the first body to make (with the setting "Voxel units" off, none).
 */
VoxelUnits::VoxelUnits()
{
}

/**
 * Stops the builder's threads, if any were started (each after the body it's making, if any).
 */
VoxelUnits::~VoxelUnits()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stopping = true;
	}
	_wake.notify_all();
	for (std::thread &builder : _builders) builder.join();
}

/**
 * How many threads make bodies at once: the machine's, less two left to the game and its renderer; six at most (a
 * fine body takes a second or more to make, and tens of megabytes while it's made), one at least.
 * @return The number.
 */
size_t VoxelUnits::builderThreads()
{
	return (size_t)std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 6);
}

/**
 * Forgets every body, and those being made (a new battle: what the builder makes of the old one's is dropped).
 */
void VoxelUnits::clear()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_jobs.clear();
		_made.clear();
		++_battle;
	}
	_models.clear();
	_byFrames.clear();
	_byQuick.clear();
	_making.clear();
	_waiting.clear();
	_lastBodies.clear();
	_lastFacings.clear();
	_restFrames.clear();
}

/**
 * The builder's loop, on its own thread: makes the bodies queued, one at a time, and hands them over (see collect),
 * until stopped. (The grid a body is made on is the builder's alone: see useGrid.)
 */
void VoxelUnits::build()
{
	// (spare time only, as the upscaler's: the frame's threads come first while bodies are made mid-battle)
#ifdef __linux__
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
	for (;;)
	{
		std::unique_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_wake.wait(lock, [this] { return _stopping || !_jobs.empty(); });
			if (_stopping) return;
			job = std::move(_jobs.front());
			_jobs.pop_front();
			++_building;
		}
		PartFrames parts[PartCount]; // (worked on: the job's stay as captured, for dump)
		std::copy(std::begin(job->parts), std::end(job->parts), std::begin(parts));
		std::shared_ptr<VoxelUnitModel> model;
		if (job->fine)
		{
			// (twice as fine: the other builder, on the frames enlarged twice: see FirstPersonVoxelBodiesFine.h)
			FineBodySource source;
			for (int p = 0; p < PartCount; ++p)
				for (int d = 0; d < 8; ++d) source.parts[p][d] = job->parts[p].frame[d];
			for (int d = 0; d < 8; ++d) source.composite[d] = job->composite[d];
			for (int hand = 0; hand < 2; ++hand)
			{
				source.itemPictures[hand] = job->items[hand].picture;
				source.itemWidth[hand] = job->items[hand].width;
				source.itemHeight[hand] = job->items[hand].height;
				source.itemTwoHanded[hand] = job->items[hand].twoHanded;
				source.itemMelee[hand] = job->items[hand].melee;
				source.itemShare[hand] = job->items[hand].share;
			}
			source.deaths = job->deaths;
			source.corpse = job->corpse;
			source.lyingPicture = job->lying.picture;
			source.lyingWidth = job->lying.width;
			source.lyingHeight = job->lying.height;
			for (int phase = 0; phase < 8; ++phase)
				for (int view = 0; view < 2; ++view)
				{
					source.walkLegs[phase][view] = job->walks.legs[phase][view];
					for (int side = 0; side < 2; ++side) source.walkArms[phase][view][side] = job->walks.arms[phase][view][side];
				}
			source.walkAny = job->walks.any;
			source.aiming = job->aiming;
			source.still = job->still;
			source.beast = job->beast;
			source.torsoDepth = job->torsoDepth;
			source.backTop = job->backTop;
			source.frontal = job->frontal;
			source.kneeling = job->kneeled;
			source.standard = job->standard;
			source.sides = job->sides;
			source.traits = job->traits;
			source.relief = job->relief;
			source.arms = job->arms;
			source.tankHull = job->tankHull;
			source.tankTop = job->tankTop;
			source.flatTop = job->flatTop;
			source.flatUnder = job->flatUnder;
			source.routine = job->routine->number;
			model = makeFineModel(source, job->palette);
		}
		else
		{
			Routine routine = *job->routine;
			routine.still = job->still;
			routine.beast = job->beast;
			routine.torsoDepth = job->torsoDepth;
			routine.backTop = job->backTop;
			routine.frontal = job->frontal;
			routine.kneeling = job->kneeled;
			routine.standard = job->standard;
			routine.sides = job->sides;
			routine.traits = job->traits;
			routine.relief = job->relief;
			routine.arms = job->arms;
			routine.tankHull = job->tankHull;
			routine.tankTop = job->tankTop;
			routine.flatTop = job->flatTop;
			routine.flatUnder = job->flatUnder;
			model = makeModel(parts, job->composite, job->items, job->deaths, job->corpse, job->lying, job->walks, job->aiming, routine, job->palette);
		}
		if (model)
		{
			model->opacity = job->shows;
			model->sparks = job->sparks;
			float lowest = 1e9f;
			for (size_t voxel = 0; voxel < model->up.size(); ++voxel)
			{
				if (model->part[voxel] != VoxelUnitModel::Item) lowest = std::min(lowest, model->up[voxel] * model->unit);
			}
			model->lowest = lowest < 1e8f ? lowest : 0.0f;
		}
		// (a body for each frame of its sprite's own animation: see modelFor; each made as the unit's own body is, from
		// that frame's 8 drawings, and kept as a shape of its own, as the fall's are)
		for (size_t frame = 0; model && (frame + 1) * 8 <= job->anim.size(); ++frame)
		{
			std::unique_ptr<VoxelUnitModel> shape;
			if (job->fine)
			{
				FineBodySource source;
				for (int d = 0; d < 8; ++d) source.parts[0][d] = job->anim[frame * 8 + d];
				source.still = job->still;
				source.beast = job->beast;
				source.torsoDepth = job->torsoDepth;
				source.backTop = job->backTop;
				source.frontal = job->frontal;
				source.kneeling = job->kneeled;
				source.standard = job->standard;
				source.sides = job->sides;
				source.traits = job->traits;
				source.relief = job->relief;
				source.flatTop = job->flatTop;
				source.flatUnder = job->flatUnder;
				source.routine = job->routine->number;
				shape = makeFineModel(source, job->palette);
			}
			else
			{
				PartFrames framed[PartCount];
				for (PartFrames &part : framed)
					for (auto &direction : part.frame) direction.assign((size_t)FrameSize * FrameSize, 0);
				for (int d = 0; d < 8; ++d) framed[0].frame[d] = job->anim[frame * 8 + d];
				framed[0].any = true;
				Routine routine = *job->routine;
				routine.still = job->still;
				routine.beast = job->beast;
				routine.torsoDepth = job->torsoDepth;
				routine.backTop = job->backTop;
				routine.frontal = job->frontal;
				routine.kneeling = job->kneeled;
				routine.standard = job->standard;
				routine.sides = job->sides;
				routine.traits = job->traits;
				routine.relief = job->relief;
				routine.flatTop = job->flatTop;
				routine.flatUnder = job->flatUnder;
				const std::vector<Uint8> noComposite[8];
				const HeldItem noItems[2];
				shape = makeModel(framed, noComposite, noItems, {}, {}, HeldItem(), WalkFrames(), false, routine, job->palette);
			}
			if (!shape)
			{
				model->frames.clear(); // (a frame with nothing to make a body of: the unit stays as it stands)
				break;
			}
			VoxelUnitModel::FallShape made;
			made.right = std::move(shape->right);
			made.ahead = std::move(shape->ahead);
			made.up = std::move(shape->up);
			made.colour.resize(made.right.size());
			for (size_t k = 0; k < made.colour.size(); ++k) made.colour[k] = shape->colours[k * 16];
			made.hits.assign(shape->hits.size(), 0);
			for (size_t k = 0; k < shape->hits.size(); ++k) made.hits[k] = shape->hits[k] != 0;
			std::copy(shape->hitFrom, shape->hitFrom + 3, made.hitFrom);
			std::copy(shape->hitSize, shape->hitSize + 3, made.hitSize);
			model->frames.push_back(std::move(made));
		}
		if (const char *folder = std::getenv("OXC3D_VOXELDUMP")) dump(*job, model.get(), folder);
		std::lock_guard<std::mutex> lock(_mutex);
		--_building;
		if (job->battle == _battle) _made.push_back({ job->hash, model });
	}
}

/**
 * A developer's aid (the environment variable OXC3D_VOXELDUMP names a folder): writes what a body was made from and
 * what it is, as unit<id>_<status>[k] (k: kneeling) and these endings: .frames (the parts' frames, 6 parts x 8
 * directions x 64x64, then the two items' pictures: width, height, pixels), .deaths (the death frames and the corpse's
 * picture), .walkframes (the walk's frames: phase x view x legs, right arm, left arm), .composite (the parts laid over
 * each other, as the game draws them, for each direction: a unit drawn in parts), .job (its drawing routine, pose and
 * items, its death frames' count and the palette), and the body (see writeModel: .points, .walk, .fall). The viewers
 * in tools/voxel_units read them, and tools/voxel_units/rebuild makes the body again from them.
 * @param job What the body was made from.
 * @param model The body (or null: none could be made).
 * @param folder Where to write.
 */
void VoxelUnits::dump(const Job &job, const VoxelUnitModel *model, const char *folder)
{
	const std::string base = std::string(folder) + "/unit" + std::to_string(job.unitId) + "_" + std::to_string(job.status) + (job.kneeled ? "k" : "");
	if (FILE *file = std::fopen((base + ".frames").c_str(), "wb"))
	{
		for (const PartFrames &part : job.parts)
			for (const auto &frame : part.frame) std::fwrite(frame.data(), 1, frame.size(), file);
		for (const HeldItem &held : job.items)
		{
			const int size[2] = { held.width, held.height };
			std::fwrite(size, sizeof(int), 2, file);
			if (!held.picture.empty()) std::fwrite(held.picture.data(), 1, held.picture.size(), file);
		}
		std::fclose(file);
	}
	if (FILE *file = std::fopen((base + ".deaths").c_str(), "wb"))
	{
		for (const auto &death : job.deaths) std::fwrite(death.data(), 1, death.size(), file);
		if (!job.corpse.empty()) std::fwrite(job.corpse.data(), 1, job.corpse.size(), file);
		std::fclose(file);
	}
	if (!job.lying.picture.empty())
	{
		if (FILE *file = std::fopen((base + ".lying").c_str(), "wb"))
		{
			const int size[2] = { job.lying.width, job.lying.height };
			std::fwrite(size, sizeof(int), 2, file);
			std::fwrite(job.lying.picture.data(), 1, job.lying.picture.size(), file);
			std::fclose(file);
		}
	}
	if (!job.composite[0].empty())
	{
		if (FILE *file = std::fopen((base + ".composite").c_str(), "wb"))
		{
			for (const auto &frame : job.composite) std::fwrite(frame.data(), 1, frame.size(), file);
			std::fclose(file);
		}
	}

	if (FILE *file = std::fopen((base + ".job").c_str(), "w"))
	{
		std::fprintf(file, "routine %d whole %d big %d aiming %d twoHanded %d %d deaths %d corpse %d beast %d torso %d top %d frontal %d relief %d melee %d %d arms %d tank %d %d standard %d sides %d traits %d share %d %d flat %d %d\n", job.routine->number, job.routine->whole, job.routine->big,
			job.aiming, job.items[0].twoHanded, job.items[1].twoHanded, (int)job.deaths.size(), (int)!job.corpse.empty(), (int)job.beast, (int)std::lround(job.torsoDepth * 100.0f), (int)job.backTop, (int)job.frontal, (int)job.relief, (int)job.items[0].melee, (int)job.items[1].melee, job.arms, (int)job.tankHull, (int)job.tankTop, (int)job.standard, (int)job.sides, job.traits, (int)std::lround(job.items[0].share * 100.0f), (int)std::lround(job.items[1].share * 100.0f), (int)std::lround(job.flatTop * 100.0f), (int)std::lround(job.flatUnder * 100.0f));
		for (int i = 0; i < 256; ++i) std::fprintf(file, "%d %d %d\n", job.palette[i].r, job.palette[i].g, job.palette[i].b);
		std::fclose(file);
	}
	if (job.walks.any)
	{
		if (FILE *file = std::fopen((base + ".walkframes").c_str(), "wb"))
		{
			const std::vector<Uint8> none(FrameSize * FrameSize, 0);
			for (int phase = 0; phase < 8; ++phase)
				for (int view = 0; view < 2; ++view)
				{
					const std::vector<Uint8> *frames[3] = { &job.walks.legs[phase][view], &job.walks.arms[phase][view][0], &job.walks.arms[phase][view][1] };
					for (const auto *frame : frames) std::fwrite(frame->empty() ? none.data() : frame->data(), 1, FrameSize * FrameSize, file);
				}
			std::fclose(file);
		}
	}
	if (model) writeModel(*model, base);
}

/**
 * Takes the bodies the builder has made since last time (none made, for frames that can't make one: kept as such, so
 * the unit stays its sprite).
 */
void VoxelUnits::collect()
{
	std::vector<std::pair<Uint64, std::shared_ptr<VoxelUnitModel>>> made;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		made.swap(_made);
	}
	for (auto &body : made)
	{
		_making.erase(body.first);
		_byFrames[body.first] = std::move(body.second);
	}
}

}
// Thanks mom and dad! You were the ones who exposed me to my first comp.
