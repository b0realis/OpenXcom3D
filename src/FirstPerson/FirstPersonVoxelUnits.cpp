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
#include <string>
#include <utility>
#include "FirstPersonVoxelBodies.h"
#include "FirstPersonVoxelBodiesFine.h"
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
	bool beast = false; // (a long body on four legs, by the unit table: see Routine)
	float torsoDepth = 0.0f; // (the torso's slices at most this deep for their width, by the unit table: see Routine)
	bool backTop = false; // (what faces up painted by the back drawing, by the unit table: see Routine)
	bool frontal = false; // (kept to its front and back drawings' outlines, by the unit table: see Routine)
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
 * The key a unit's body is kept under: the unit, its armour, kneeling, aiming, flying, what it holds, and a big unit's
 * turret's turn from its hull.
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
	return Key(unit->getId(), unit->getArmor(), !standing && unit->isKneeled(), !standing && unit->getStatus() == STATUS_AIMING,
		!standing && unit->isFloating(), held(unit->getRightHandWeapon()), held(unit->getLeftHandWeapon()), turret);
}

/**
 * The body for a unit as it is now, made from its sprites the first time it's seen so (a unit drawn just like one
 * seen before shares its body). A walking unit takes the body it stands in (and the walk's phase), if it was ever
 * seen standing; else there's none yet (it's drawn as its sprite meanwhile).
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
		if (made == _byFrames.end()) return nullptr; // (still being made)
		_models[key] = made->second;
		_waiting.erase(waiting);
		return remember(unit, made->second);
	}
	if (builds <= 0 || !palette) return nullptr;

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
	for (int d = 0; d < 8; ++d)
	{
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
		for (int p = 0; p < PartCount; ++p)
		{
			std::vector<Uint8> &pixels = parts[p].frame[d];
			pixels.resize(FrameSize * FrameSize);
			for (int row = 0; row < FrameSize; ++row)
			{
				for (int column = 0; column < FrameSize; ++column)
				{
					pixels[row * FrameSize + column] = frames[p]->getPixel(column, row);
					parts[p].any |= pixels[row * FrameSize + column] != 0;
				}
			}
		}
		if (!routine->big && !routine->whole)
		{
			layered.clear();
			unitSprite.setPartCapture(together);
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.setPartCapture(nullptr);
			composite[d].resize(FrameSize * FrameSize);
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) composite[d][row * FrameSize + column] = layered.getPixel(column, row);
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
			std::vector<Uint8> pixels(FrameSize * FrameSize);
			bool any = false;
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) any |= (pixels[row * FrameSize + column] = frames[0]->getPixel(column, row)) != 0;
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
		auto pixelsOf = [](Surface *surface) {
			std::vector<Uint8> pixels(FrameSize * FrameSize);
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) pixels[row * FrameSize + column] = surface->getPixel(column, row);
			return pixels;
		};
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
	unitSprite.setViewRotation(0);
	unitSprite.setDrawOut(false);
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
			corpse.resize(FrameSize * FrameSize);
			bool any = false;
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) any |= (corpse[row * FrameSize + column] = frames[0]->getPixel(column, row)) != 0;
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
		const Surface *frame = inventorySprites ? item->getBigSprite(inventorySprites, save, 0) : nullptr;
		if (!frame) continue;
		Surface picture(frame->getWidth(), frame->getHeight());
		ScriptWorkerBlit worker;
		BattleItem::ScriptFill(&worker, item, save, BODYPART_ITEM_INVENTORY, 0, 0);
		worker.executeBlit(frame, &picture, 0, 0, 0);
		items[hand].width = picture.getWidth();
		items[hand].height = picture.getHeight();
		items[hand].picture.resize(items[hand].width * items[hand].height);
		for (int y = 0; y < items[hand].height; ++y)
			for (int x = 0; x < items[hand].width; ++x) items[hand].picture[y * items[hand].width + x] = picture.getPixel(x, y);
	}

	// A unit drawn just like one before (the same frames, pictures and pose) shares its body.
	const bool aiming = unit->getStatus() == STATUS_AIMING;
	Uint64 hash = 14695981039346656037ULL;
	for (const PartFrames &part : parts)
		for (const auto &frame : part.frame) hashBytes(hash, frame.data(), frame.size());
	for (const HeldItem &held : items)
	{
		hashBytes(hash, held.picture.data(), held.picture.size());
		const Uint8 flags[3] = { (Uint8)held.width, (Uint8)held.height, (Uint8)held.twoHanded };
		hashBytes(hash, flags, 3);
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
	// What the mod's rules say of the unit by its names (the unit table: a ghost seen through, a beast on four legs).
	const Unit *unitRules = unit->getUnitRules();
	const Soldier *soldier = unit->getGeoscapeSoldier();
	const RuleFirstPerson::UnitLook look = save->getMod()->getFirstPerson().getUnitLook({ unit->getType(), unit->getArmor()->getType(),
		unit->getArmor()->getSpriteSheet(), unitRules ? unitRules->getRace() : std::string(), soldier ? soldier->getRules()->getType() : std::string() });
	float shows = dithered ? VoxelDitherFaint : 1.0f;
	if (look.shows >= 0) shows = std::min(shows, look.shows / 100.0f);
	const Uint8 pose[8] = { (Uint8)aiming, (Uint8)routine->number, (Uint8)FineBodies, (Uint8)std::lround(shows * 100.0f), (Uint8)(look.beast == 1),
		(Uint8)std::max(look.torsoDepth, 0), (Uint8)(look.backTop == 1), (Uint8)(look.frontal == 1) };
	hashBytes(hash, pose, 8);
	auto alike = _byFrames.find(hash);
	if (alike != _byFrames.end())
	{
		_models[key] = alike->second;
		return remember(unit, alike->second);
	}
	// Else the builder makes it (unless it's making one alike already), and the unit is drawn as its sprite meanwhile.
	--builds;
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
		job->beast = look.beast == 1;
		job->torsoDepth = look.torsoDepth > 0 ? look.torsoDepth / 100.0f : 0.0f;
		job->backTop = look.backTop == 1;
		job->frontal = look.frontal == 1;
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
	return nullptr;
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
	_making.clear();
	_waiting.clear();
	_lastBodies.clear();
	_lastFacings.clear();
}

/**
 * The builder's loop, on its own thread: makes the bodies queued, one at a time, and hands them over (see collect),
 * until stopped. (The grid a body is made on is the builder's alone: see useGrid.)
 */
void VoxelUnits::build()
{
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
			model = makeModel(parts, job->composite, job->items, job->deaths, job->corpse, job->lying, job->walks, job->aiming, routine, job->palette);
		}
		if (model) model->opacity = job->shows;
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
		std::fprintf(file, "routine %d whole %d big %d aiming %d twoHanded %d %d deaths %d corpse %d beast %d torso %d top %d frontal %d\n", job.routine->number, job.routine->whole, job.routine->big,
			job.aiming, job.items[0].twoHanded, job.items[1].twoHanded, (int)job.deaths.size(), (int)!job.corpse.empty(), (int)job.beast, (int)std::lround(job.torsoDepth * 100.0f), (int)job.backTop, (int)job.frontal);
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
