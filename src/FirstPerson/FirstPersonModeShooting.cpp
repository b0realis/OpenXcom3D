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
 * FirstPersonModeShooting.cpp: weapons, aiming and firing.
 *
 * The weapon in the chosen hand and its fire modes, what's under the crosshair, firing through
 * the game's own actions (time units, accuracy, line of fire, reaction fire), the optional
 * FPS-style spread and recoil, the crosshair with the shot's odds and cost, tactical flares,
 * and aiming guided missiles and psi on the isometric map.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/TileEngine.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Engine/Sound.h"
#include "../Interface/Text.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Finds the weapon the soldier fires with: the one in the hand the player chose (G switches
 * hands), or the one in the other hand if the chosen hand is empty.
 * @param unit The soldier.
 * @return The weapon, or null if both hands are empty.
 */
BattleItem *FirstPersonMode::activeWeapon(BattleUnit *unit) const
{
	// The hand this soldier was last switched to; every soldier starts with the right hand.
	auto handEntry = _chosenHand.find(unit);
	const int hand = handEntry != _chosenHand.end() ? handEntry->second : 0; // 0 right, 1 left
	BattleItem *rightHandWeapon = unit->getRightHandWeapon(), *leftHandWeapon = unit->getLeftHandWeapon();

	// Take the chosen hand's weapon; with that hand empty, fall back to the other hand.
	BattleItem *chosenWeapon = hand ? leftHandWeapon : rightHandWeapon;
	return chosenWeapon ? chosenWeapon : (hand ? rightHandWeapon : leftHandWeapon);
}

/**
 * Dual wield (an option): finds the gun in the other hand, which is drawn on its own side of
 * the view and fires together with the chosen one.
 * Both weapons must be firearms; a guided launcher (one with waypoints, like the blaster
 * launcher) doesn't count, since its missile is steered on the isometric map.
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @return The other hand's gun, or null if dual wield is off or either weapon isn't a gun.
 */
BattleItem *FirstPersonMode::offHandWeapon(BattleUnit *unit, const BattleItem *weapon) const
{
	// The chosen weapon itself must be a plain gun, and the option must be on.
	if (!Options::fpsDualWield || !unit || !weapon || weapon->getRules()->getBattleType() != BT_FIREARM || weapon->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}

	// The same test for whatever is in the other hand (and it must be a second item).
	BattleItem *otherWeapon = weapon == unit->getRightHandWeapon() ? unit->getLeftHandWeapon() : unit->getRightHandWeapon();
	if (!otherWeapon || otherWeapon == weapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM || otherWeapon->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}
	return otherWeapon;
}

/**
 * Dual wield: finds the other hand's gun if it can fire along with the chosen gun's fire mode,
 * and works out which of its own modes it fires: the same mode if it has it, else its first
 * shot mode. Each gun pays its own time units for its own mode (by the game's rules).
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @param slot The fire mode picked for the chosen weapon (before FPS style turns a quick
 *        snap shot into a burst, see firedAs).
 * @param partnerMode Set to the fire mode the other gun fires.
 * @return The other hand's gun, or null if it can't join in (no gun, no shot mode that fits,
 *         or no ammo).
 */
BattleItem *FirstPersonMode::dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &partnerMode) const
{
	// Only shots are fired with both hands (not throws, melee, psi...).
	BattleItem *otherWeapon = isShot(slot) ? offHandWeapon(unit, weapon) : nullptr;
	if (!otherWeapon)
	{
		return nullptr;
	}

	// Pick the other gun's mode: the same as the chosen gun's if it has it, else its first shot.
	const std::vector<BattleActionType> otherModes = fireModesFor(unit, otherWeapon);
	BattleActionType pickedMode = BA_NONE;
	if (std::find(otherModes.begin(), otherModes.end(), slot) != otherModes.end())
	{
		pickedMode = slot;
	}
	else
	{
		for (BattleActionType candidate : otherModes)
		{
			if (isShot(candidate)) { pickedMode = candidate; break; }
		}
	}
	if (pickedMode == BA_NONE)
	{
		return nullptr;
	}
	partnerMode = firedAs(otherWeapon, pickedMode);

	// Its rounds fly beside this gun's, round for round, so it may fire no more rounds than
	// this one does: if its mode fires more, fall back to its first mode that fires few enough.
	const int chosenShots = weapon->getActionConf(firedAs(weapon, slot))->shots;
	if (otherWeapon->getActionConf(partnerMode)->shots > chosenShots)
	{
		partnerMode = BA_NONE;
		for (BattleActionType candidate : { BA_SNAPSHOT, BA_AIMEDSHOT, BA_AUTOSHOT })
		{
			if (std::find(otherModes.begin(), otherModes.end(), candidate) != otherModes.end()
				&& otherWeapon->getActionConf(candidate)->shots <= chosenShots) { partnerMode = candidate; break; }
		}
		if (partnerMode == BA_NONE)
		{
			return nullptr;
		}
	}

	// It joins in only if it's loaded for that mode.
	// (Note: partnerMode stays set even when this returns null for want of ammo.)
	return otherWeapon->getAmmoForAction(partnerMode) ? otherWeapon : nullptr;
}

/**
 * Lists what the left mouse button can do with an item, in the order the mouse wheel cycles
 * through them. It mirrors the game's own action menu (ActionMenuState): the same actions
 * under the same conditions, so the 3D view offers nothing the game wouldn't. (Unpriming and
 * the mana requirement some mods use are not mirrored.)
 * @param unit The soldier holding the item.
 * @param weapon The item (null gives an empty list).
 * @return The fire modes (actions) in menu order.
 */
std::vector<BattleActionType> FirstPersonMode::fireModesFor(BattleUnit *unit, BattleItem *weapon) const
{
	std::vector<BattleActionType> modes;
	if (!weapon)
	{
		return modes;
	}
	const RuleItem *rules = weapon->getRules();

	// As in the game's menu, an item that needs psi skill can only be thrown by a soldier
	// without any.
	const bool psiBlocked = rules->isPsiRequired() && unit->getBaseStats()->psiSkill <= 0;
	if (!psiBlocked)
	{
		// A gun: each shot mode that has a time cost. On a guided launcher (it has waypoints)
		// the launch replaces the snap and auto shots that would use the same ammo slot, and
		// there's no aimed shot.
		if (rules->getBattleType() == BT_FIREARM)
		{
			const bool isLauncher = weapon->getCurrentWaypoints() != 0;
			const int launchAmmoSlot = weapon->getActionConf(BA_LAUNCH)->ammoSlot;
			if ((!isLauncher || launchAmmoSlot != weapon->getActionConf(BA_SNAPSHOT)->ammoSlot) && rules->getCostSnap().Time > 0) modes.push_back(BA_SNAPSHOT);
			if (!isLauncher && rules->getCostAimed().Time > 0) modes.push_back(BA_AIMEDSHOT);
			if ((!isLauncher || launchAmmoSlot != weapon->getActionConf(BA_AUTOSHOT)->ammoSlot) && rules->getCostAuto().Time > 0) modes.push_back(BA_AUTOSHOT);
			if (isLauncher) modes.push_back(BA_LAUNCH);
		}

		// Any item with a melee cost can hit (a stun rod, a knife...).
		if (rules->getCostMelee().Time > 0) modes.push_back(BA_HIT);

		// A psi amp: panic, mind control and its own use, each if it has a time cost. Medi-kits,
		// motion scanners and mind probes are used from their own menu.
		if (rules->getBattleType() == BT_PSIAMP)
		{
			if (rules->getCostPanic().Time > 0) modes.push_back(BA_PANIC);
			if (rules->getCostMind().Time > 0) modes.push_back(BA_MINDCONTROL);
			if (rules->getCostUse().Time > 0) modes.push_back(BA_USE);
		}
		else if (rules->getBattleType() == BT_MEDIKIT || rules->getBattleType() == BT_SCANNER || rules->getBattleType() == BT_MINDPROBE)
		{
			modes.push_back(BA_USE);
		}
	}

	// Anything not built in can be thrown; a grenade not yet primed can be primed.
	if (!rules->isFixed() && rules->getCostThrow().Time > 0) modes.push_back(BA_THROW);
	if (!psiBlocked && rules->getFuseTimerType() != BFT_NONE && weapon->getFuseTimer() == -1 && rules->getCostPrime().Time > 0) modes.push_back(BA_PRIME);
	return modes;
}

/**
 * Keeps the fire mode valid for the weapon in hand, every frame of the player's turn. A fire
 * mode picked on the classic HUD (its weapon buttons and action menu) is taken over, hand and
 * all; otherwise a new weapon, or a mode the weapon no longer has, falls back to its first mode.
 */
void FirstPersonMode::updateFireMode(BattleUnit *unit)
{
	BattleAction *action = _state->_battleGame->getCurrentAction();

	// The game's current action is being aimed by this soldier: that mode was picked on the
	// classic HUD, so take it over. Not, though, the auto shot that a BURST (FPS style: the
	// quick slot fires an auto burst) fired just now: the quick slot stays picked.
	const bool ownBurst = action->type != _fireMode && action->type == firedAs(action->weapon, _fireMode);
	if (action->targeting && action->actor == unit && action->weapon && action->type != BA_NONE && !ownBurst)
	{
		_chosenHand[unit] = action->weapon == unit->getLeftHandWeapon() ? 1 : 0;
		_fireMode = action->type;
		_fireModeWeapon = action->weapon;
		return;
	}

	// Otherwise keep the mode while the weapon has it; a different weapon, or a mode that's
	// gone (a grenade just primed, say), starts again at the weapon's first mode.
	BattleItem *weapon = activeWeapon(unit);
	const std::vector<BattleActionType> modes = fireModesFor(unit, weapon);
	if (weapon != _fireModeWeapon || std::find(modes.begin(), modes.end(), _fireMode) == modes.end())
	{
		_fireMode = modes.empty() ? BA_NONE : modes.front();
		_fireModeWeapon = weapon;
	}
}

/**
 * Steps to the next or previous fire mode of the weapon in hand (mouse wheel, or F by
 * default), wrapping around at either end. Anything being aimed on the game's side is called
 * off first.
 * @param step +1 for the next mode, -1 for the previous one.
 */
void FirstPersonMode::cycleFireMode(int step)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canAct())
	{
		return;
	}
	_state->_battleGame->cancelCurrentAction();
	updateFireMode(unit);
	const std::vector<BattleActionType> modes = fireModesFor(unit, activeWeapon(unit));
	if (modes.empty())
	{
		return;
	}

	// Move from the current mode's place in the list, wrapping around both ways.
	const int count = (int)modes.size();
	const int currentIndex = (int)(std::find(modes.begin(), modes.end(), _fireMode) - modes.begin());
	_fireMode = modes[((currentIndex + step) % count + count) % count];
}

/**
 * Picks a fire mode directly (the snap, aimed and auto keys), if the weapon in hand has it.
 * @param mode The fire mode wanted.
 */
void FirstPersonMode::selectFireMode(BattleActionType mode)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canAct())
	{
		return;
	}

	// Only a mode the weapon offers; picking it calls off anything being aimed.
	const std::vector<BattleActionType> modes = fireModesFor(unit, activeWeapon(unit));
	if (std::find(modes.begin(), modes.end(), mode) != modes.end())
	{
		_state->_battleGame->cancelCurrentAction();
		_fireMode = mode;
		_fireModeWeapon = activeWeapon(unit);
	}
}

/**
 * Switches the soldier's weapon hand (G by default). The fire mode is picked again for the new
 * weapon on the next frame (see updateFireMode). It costs nothing: on the classic HUD either
 * hand's weapon can be used at any time too.
 */
void FirstPersonMode::switchHand()
{
	if (!canAct())
	{
		return;
	}
	_state->_battleGame->cancelCurrentAction();
	if (BattleUnit *unit = _save->getSelectedUnit())
	{
		_chosenHand[unit] ^= 1; // 0 right <-> 1 left
	}
	_fireModeWeapon = nullptr; // forces a fresh fire mode for whatever is in the new hand
}

/**
 * Finds what a shot would hit through a pixel of the view: the first voxel along that ray,
 * found by the game's own line of fire (TileEngine::calculateLineVoxel, the same 3D Bresenham
 * line through the same voxel shapes a real shot flies along). Walls, objects, floors and units
 * all stop it, except the soldier himself; units the player can't see are passed through
 * (unless the game is in debug mode), so the crosshair gives nothing away. Off into the sky,
 * it gives a point some way along the ray instead.
 * @param pixelX Pixel column in the 3D view.
 * @param pixelY Pixel row in the 3D view.
 * @param unit The soldier looking (left out of the line).
 * @param voxel Set to the voxel hit, or to a point 320 voxels (20 tiles) along the ray.
 * @return True if something was hit.
 */
bool FirstPersonMode::aimAt(int pixelX, int pixelY, BattleUnit *unit, Position &voxel)
{
	// The ray from the eye through that pixel, in voxels (16 per tile across, 24 per level).
	double eye[3], rayDirection[3];
	_view->getEye(eye);
	_view->getRayDirection(pixelX, pixelY, rayDirection);
	const Position rayOrigin((int)eye[0], (int)eye[1], (int)eye[2]);
	const Position farPoint(rayOrigin.x + (int)(rayDirection[0] * 1500), rayOrigin.y + (int)(rayDirection[1] * 1500),
		rayOrigin.z + (int)(rayDirection[2] * 1500));

	// Walk the game's line of fire out to 1500 voxels. Without storing the whole trajectory the
	// game keeps only the point where the line stopped. The arguments after the trajectory:
	// leave the soldier out, test every unit, and skip units the player can't see.
	std::vector<Position> trajectory;
	const int voxelType = _save->getTileEngine()->calculateLineVoxel(rayOrigin, farPoint, false, &trajectory, unit, nullptr, !_save->getDebugMode());
	if (voxelType != V_EMPTY && voxelType != V_OUTOFBOUNDS && !trajectory.empty())
	{
		voxel = trajectory.front(); // a floor, a wall, an object or a unit was hit
		return true;
	}

	// Nothing hit (the sky, or out of the map): a point 20 tiles along the ray.
	voxel = Position(rayOrigin.x + (int)(rayDirection[0] * 320), rayOrigin.y + (int)(rayDirection[1] * 320),
		rayOrigin.z + (int)(rayDirection[2] * 320));
	return false;
}

/**
 * Works out what's under the crosshair (the centre of the view) this frame, for the hit
 * chance and range shown under it.
 */
void FirstPersonMode::updateAim(BattleUnit *unit)
{
	_aimHitsSomething = aimAt(_view->getWidth() / 2, _view->getHeight() / 2, unit, _aimVoxel);
}

/**
 * Uses the fire mode on whatever is under a pixel of the view (the crosshair with mouse look,
 * the cursor otherwise). Everything goes through the game's own actions, exactly as a click
 * on the isometric map would: the game charges the time units, rolls the accuracy (in FPS
 * style the spread takes the roll's place, see below), flies the shot along its line of fire
 * and triggers reaction fire.
 * - Shots and throws target the tile under the pixel (or its exact voxel, see below).
 * - A guided launcher and psi on a creature out of sight are aimed on the isometric map.
 * - Melee hits whatever is in front of the soldier.
 * - Medi-kits, scanners and priming open the item's usual action menu.
 * @param pixelX Pixel column in the 3D view.
 * @param pixelY Pixel row in the 3D view.
 */
void FirstPersonMode::fire(int pixelX, int pixelY)
{
	// Only a soldier of ours who may act now.
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canAct() || !_state->playableUnitSelected())
	{
		return;
	}
	updateFireMode(unit);
	BattleItem *weapon = activeWeapon(unit);
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();
	BattleActionType mode = _fireMode;

	// An action picked from an item's menu that wants a target (the mind probe, psi picked from
	// the menu...): this click is its target, with that item and action.
	const bool menuTargeting = action->targeting && action->actor == unit && action->weapon;
	if (menuTargeting)
	{
		weapon = action->weapon;
		mode = action->type;
	}
	if (!weapon || mode == BA_NONE)
	{
		return;
	}

	// A guided missile's waypoints are placed on the isometric map, by the game's own clicks.
	if (mode == BA_LAUNCH)
	{
		startMapAim(unit, weapon, BA_LAUNCH, "STR_FPS_LAUNCH_HINT");
		return;
	}

	// Psi and the mind probe reach creatures out of sight: with no creature under the pixel,
	// the player picks one on the isometric map instead.
	const bool mindProbe = mode == BA_USE && menuTargeting && weapon->getRules()->getBattleType() == BT_MINDPROBE;
	if (mode == BA_PANIC || mode == BA_MINDCONTROL || mindProbe)
	{
		Position pointedVoxel;
		aimAt(pixelX, pixelY, unit, pointedVoxel);
		if (!creatureAt(pointedVoxel, unit))
		{
			startMapAim(unit, weapon, mode, "STR_FPS_PSI_HINT");
			return;
		}
	}

	// Medi-kit, scanner, priming...: the item's own action menu, as clicking it on the HUD.
	if ((mode == BA_USE || mode == BA_PRIME) && !menuTargeting)
	{
		_state->handleItemClick(weapon, false);
		return;
	}

	// Melee: the game finds who is in reach in the direction the soldier faces (the 3D view
	// keeps him facing where it looks) and does the attack as its own action, which needs no
	// target click.
	if (mode == BA_HIT)
	{
		Position meleeTile;
		if (!_save->getTileEngine()->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &meleeTile))
		{
			_state->warning("STR_THERE_IS_NO_ONE_THERE");
			return;
		}
		action->actor = unit;
		action->weapon = weapon;
		action->type = BA_HIT;
		action->targeting = false;
		action->target = meleeTile;
		action->result.clear();
		action->updateTU();
		battleGame->handleNonTargetAction();

		// The weapon in hand swings only if the game went ahead: refused (not enough time
		// units), the action carries the reason in its result.
		if (action->result.empty())
		{
			_swingStart = SDL_GetTicks();
			_swingJolted = false;
			_sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f);
		}
		return;
	}

	// Shots and throws from here on. FPS style turns the quick slot (snap) into a burst (the
	// game's auto, see firedAs); "slot" keeps what was picked. An empty gun clicks.
	const BattleActionType slot = mode;
	mode = firedAs(weapon, mode);
	if (isShot(mode) && !weapon->getAmmoForAction(mode))
	{
		_sounds->play(FirstPersonSounds::DRY_FIRE); // the game refuses the shot itself
	}

	// The target tile: the one holding the voxel under the pixel, kept inside the map (a point
	// in the sky may lie beyond its edge or above its top).
	Position aimedVoxel;
	const bool hitSomething = aimAt(pixelX, pixelY, unit, aimedVoxel);
	Position targetTile = aimedVoxel.toTile();
	targetTile.x = std::clamp<int>(targetTile.x, 0, _save->getMapSizeX() - 1);
	targetTile.y = std::clamp<int>(targetTile.y, 0, _save->getMapSizeY() - 1);
	targetTile.z = std::clamp<int>(targetTile.z, 0, _save->getMapSizeZ() - 1);

	// Set up the game's current action as its own targeting would, and let it work out the
	// cost in time units.
	action->actor = unit;
	action->weapon = weapon;
	action->type = mode;
	action->targeting = true;
	action->waypoints.clear();
	action->updateTU();

	// Two things the add-on adds to the action, both read by the game's own shot code
	// (ProjectileFlyBState, Projectile):
	// - freeAim (FPS style): the game asks for each round's target the moment it fires it
	//   (freeAimTarget) and rolls no accuracy; the line of fire is still the game's.
	// - targetVoxel (setting "Aim at exact point", or FPS style): aim at the very voxel under
	//   the crosshair instead of the point the game picks on the target tile (the middle of a
	//   unit, an object...); the game's accuracy roll still applies to it.
	action->freeAim = freeAimOn(mode);
	if ((Options::fpsAimExact || action->freeAim) && hitSomething && isShot(mode))
	{
		action->targetVoxel = aimedVoxel;
	}

	// Dual wield (an option): the gun in the other hand fires at the same moment, round for
	// round, both half as accurate (the game halves both shots' accuracy). With time units for
	// only one of them, just the chosen hand fires, at full accuracy. (Not for an action picked
	// from a menu.)
	BattleActionType offMode = BA_NONE;
	BattleItem *otherWeapon = menuTargeting ? nullptr : dualPartner(unit, weapon, slot, offMode);
	if (otherWeapon && !Options::fpsDoomMode && action->Time + BattleActionCost(offMode, unit, otherWeapon).Time > unit->getTimeUnits())
	{
		otherWeapon = nullptr;
	}
	action->dualWield = otherWeapon != nullptr;
	action->dualWeapon = otherWeapon;
	action->dualType = offMode;

	// Fire, as a click on the target tile of the isometric map: the game turns the soldier,
	// checks and spends the time units, and launches the shot. With the game's "confirm fire
	// mode" setting the first click only marks the target, so click a second time.
	battleGame->primaryAction(targetTile);
	if (Options::battleConfirmFireMode && action->targeting && !action->waypoints.empty())
	{
		battleGame->primaryAction(targetTile);
	}

	// Clear what we added, so the next action (one from the classic HUD, say) aims normally.
	action->targetVoxel = Position(-1, -1, -1);
	action->freeAim = false;
	action->dualWield = false;
	action->dualWeapon = nullptr;
	action->dualType = BA_NONE;
}

/**
 * Tells what a fire mode really fires. Normally just that mode; but in FPS style (setting
 * "Faithful X-COM targeting" off) the quick slot (snap) of a gun that has an auto shot fires one
 * burst instead: the game's own auto shot, with its number of rounds and its time units. (The
 * auto slot itself then keeps firing bursts while the trigger is held, see keepFiringWhileHeld.)
 * @param weapon The weapon (null: the mode as it is).
 * @param mode The fire mode picked.
 * @return The action the game is asked to do.
 */
BattleActionType FirstPersonMode::firedAs(const BattleItem *weapon, BattleActionType mode) const
{
	if (mode != BA_SNAPSHOT || !freeAimOn(mode) || !weapon)
	{
		return mode;
	}

	// A burst only from a gun with an auto shot, and not from a guided launcher.
	const RuleItem *rules = weapon->getRules();
	return rules->getCostAuto().Time > 0 && weapon->getCurrentWaypoints() == 0 ? BA_AUTOSHOT : mode;
}

/**
 * Tells whether this fire mode is FPS style just now: the setting "Faithful X-COM targeting" is
 * off and the mode is one of a gun's shots (snap, aimed, auto). Throws, melee and psi always
 * follow the game's own targeting.
 */
bool FirstPersonMode::freeAimOn(BattleActionType mode) const
{
	return !Options::fpsFaithfulTargeting && isShot(mode);
}

/**
 * Works out how wide an FPS-style shot spreads: the angle, in degrees off the crosshair, that
 * 95% of the rounds land within. Aimed shots are tight, snap shots from the hip wider, auto in
 * between (before the recoil adds to it). The soldier's firing skill and kneeling tighten it,
 * as they improve the game's own accuracy.
 * @param unit The soldier firing.
 * @param mode The shot (BA_AIMEDSHOT, BA_AUTOSHOT, or anything else for a snap shot).
 * @param dual Both guns fire at once (dual wield): twice as wide.
 * @return The 95% spread angle, in degrees.
 */
double FirstPersonMode::spreadFor(const BattleUnit *unit, BattleActionType mode, bool dual) const
{
	// The base spread by mode.
	double spreadDegrees = mode == BA_AIMEDSHOT ? 0.6 : mode == BA_AUTOSHOT ? 1.6 : 2.2;

	// Firing skill: 50 gives 1.0, a 90 marksman 0.6 (the tightest), a skill of 20 or less 1.3.
	spreadDegrees *= std::clamp(1.5 - unit->getBaseStats()->firing / 100.0, 0.6, 1.3);

	// Kneeling steadies the aim (the game gives kneeling an accuracy bonus too).
	if (unit->isKneeled()) spreadDegrees *= 0.7;

	if (dual) spreadDegrees *= 2.0; // a gun in each hand: half as accurate
	return spreadDegrees;
}

/**
 * The game's hook for an FPS-style shot (see FirstPerson::freeAimTarget): the game asks for
 * the next round's target point as it fires it. Passed on to the first-person mode if the 3D
 * view is showing; otherwise the target is left as the game had it.
 * @param state The battlescape screen (it owns the first-person mode).
 * @param action The shot being fired.
 * @param target The target point in voxels, to change.
 */
void FirstPersonMode::freeAim(BattlescapeState *state, const BattleAction &action, Position &target)
{
	if (state && state->_firstPerson && state->_firstPerson->isActive())
	{
		state->_firstPerson->freeAimTarget(action, target);
	}
}

/**
 * The game's hook asking whether a shot's bullet hits should play quicker: they do for our own
 * soldiers' shots in FPS style in the 3D view, so the next round of a burst doesn't wait for the
 * last one's puff (the game's own pace is about a round a second).
 * @param state The battlescape screen.
 * @param attacker The unit that fired.
 * @return True to play the hit quicker.
 */
bool FirstPersonMode::fastShots(BattlescapeState *state, const BattleUnit *attacker)
{
	return state && state->_firstPerson && state->_firstPerson->isActive() && !Options::fpsFaithfulTargeting
		&& attacker && attacker->getFaction() == FACTION_PLAYER;
}

/**
 * Picks the target of the next round of an FPS-style shot, the moment the game fires it: along
 * the crosshair as it points now (the recoil of the rounds before has lifted it), spread at
 * random by the mode and the soldier (spreadFor), plus the bloom of the last rounds, which then
 * grows a little for the next one. The game rolls no accuracy for such a shot; it carries the
 * line on through this point and does the rest by its own rules (the line of fire, what it
 * hits, the damage).
 * @param action The shot being fired.
 * @param target Set to the target point, in voxels.
 */
void FirstPersonMode::freeAimTarget(const BattleAction &action, Position &target)
{
	BattleUnit *unit = action.actor;
	if (!unit)
	{
		return;
	}

	// The spread as a normal distribution: 95% of a 2D normal spread lies within 2.45 sigma.
	const double spreadDegrees = spreadFor(unit, action.type, action.dualWield) + _bloom;
	const double sigmaRadians = spreadDegrees / 2.45 * Pi / 180.0;

	// A random number from the standard normal distribution (the Box-Muller method), from the
	// game's own random number generator.
	auto gaussian = [] {
		const double uniform1 = std::max(1e-9, RNG::generate(0.0, 1.0)), uniform2 = RNG::generate(0.0, 1.0);
		return std::sqrt(-2.0 * std::log(uniform1)) * std::cos(2.0 * Pi * uniform2);
	};

	// This round's angles off the crosshair (radians, right and up), turned into the pixel of
	// the view they point through (the focal length in pixels comes from the field of view).
	const double angleX = gaussian() * sigmaRadians, angleY = gaussian() * sigmaRadians;
	const int viewWidth = _view->getWidth(), viewHeight = _view->getHeight();
	const double focalPixels = (viewWidth / 2.0) / std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0);
	const int pixelX = std::clamp((int)std::lround(viewWidth / 2.0 + std::tan(angleX) * focalPixels), 0, viewWidth - 1);
	const int pixelY = std::clamp((int)std::lround(viewHeight / 2.0 - std::tan(angleY) * focalPixels), 0, viewHeight - 1);

	// The round goes for whatever the game's line of fire meets through that pixel; with
	// nothing there (the sky), for a point far along the ray.
	Position hitVoxel;
	if (aimAt(pixelX, pixelY, unit, hitVoxel))
	{
		target = hitVoxel;
	}
	else
	{
		double eye[3], rayDirection[3];
		_view->getEye(eye);
		_view->getRayDirection(pixelX, pixelY, rayDirection);
		target = Position((int)(eye[0] + rayDirection[0] * 1500), (int)(eye[1] + rayDirection[1] * 1500), (int)(eye[2] + rayDirection[2] * 1500));
	}

	// Each round blooms the spread for the next (more in auto), up to 3 degrees more.
	_bloom = std::min(_bloom + (action.type == BA_AUTOSHOT ? 0.45 : 0.3), 3.0);
}

/**
 * FPS-style recoil, every frame: each of our rounds lifts the view by the power of what it
 * fires (lasers barely, plasma a little more), with a little sideways jitter. After a pause in
 * firing the view eases back down by as much as it climbed, and the bloom fades.
 * @param unit The soldier being looked through (may be null).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::updateRecoil(BattleUnit *unit, double elapsedSeconds)
{
	const Uint32 now = SDL_GetTicks();

	// A new round of ours left the muzzle (the view stamps each with the time it fired; with
	// dual wield, either gun's round counts).
	const Uint32 shotKey = _view->getOwnShotTick() ^ (_view->getPartnerShotTick() << 1);
	if (shotKey != _recoilShotSeen)
	{
		_recoilShotSeen = shotKey;
		BattleItem *weapon = unit ? activeWeapon(unit) : nullptr;
		if (!Options::fpsFaithfulTargeting && weapon && weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			// How hard it kicks: by the power of the loaded ammo (or of the gun, if it needs
			// none), less for beams, and less for single shots than for bursts.
			const BattleItem *ammo = weapon->getAmmoForSlot(0);
			const RuleItem *ammoRules = ammo ? ammo->getRules() : weapon->getRules();
			const RuleDamageType *damageType = ammoRules->getDamageType();
			const double damageFactor = damageType && damageType->ResistType == DT_LASER ? 0.4 : damageType && damageType->ResistType == DT_PLASMA ? 0.7 : 1.0;
			const BattleActionType firedMode = firedAs(weapon, _fireMode);
			const double modeFactor = firedMode == BA_AUTOSHOT ? 1.0 : firedMode == BA_SNAPSHOT ? 0.8 : 0.6;
			const double climbDegrees = std::clamp(ammoRules->getPower() / 40.0, 0.4, 2.0) * damageFactor * modeFactor;

			// Lift the view (and jitter it sideways), remembering how far, to ease it back later.
			_view->rotate(RNG::generate(-0.35, 0.35) * climbDegrees, climbDegrees);
			_recoilDebt += climbDegrees;
			_lastRecoil = now;
		}
	}

	// 180 ms after the last kick: the view sinks back at up to 9 degrees a second, and the
	// bloom fades at 5 degrees a second.
	if (now - _lastRecoil > 180)
	{
		if (_recoilDebt > 0.0)
		{
			const double easeDegrees = std::min(_recoilDebt, 9.0 * elapsedSeconds);
			_view->rotate(0.0, -easeDegrees);
			_recoilDebt -= easeDegrees;
		}
		_bloom = std::max(0.0, _bloom - 5.0 * elapsedSeconds);
	}
}

/**
 * Draws an X-COM style crosshair for a fire mode onto the crosshair surface (CrosshairSize
 * pixels square), each point with a dark outline so it reads on any background:
 * snap = an open cross, aimed = a scope ring, auto = brackets with one dot per round of the
 * burst, throw = an arc onto a landing ring, launcher = a diamond, melee = claw marks,
 * psi = rings, items used from their menu = a box.
 * @param mode The fire mode (anything else: a small plus).
 * @param shots Rounds in an auto burst (dots under the brackets, at most six).
 * @param color Palette colour of the crosshair.
 * @param shadow Palette colour of its outline.
 * @param hitMark 0 none, 1 a hit, 2 a kill: ticks on the diagonals.
 * @param hitColor Palette colour of those ticks.
 */
void FirstPersonMode::drawCrosshair(BattleActionType mode, int shots, Uint8 color, Uint8 shadow, int hitMark, Uint8 hitColor)
{
	// The shapes are collected as points first (relative to the centre), so the outline can go
	// under all of them before the colour goes on top.
	const int centre = CrosshairSize / 2;
	std::vector<std::pair<int, int>> points;
	auto dot = [&](int x, int y) { points.push_back({ centre + x, centre + y }); };

	// A straight line between two points, pixel by pixel (Bresenham's line algorithm).
	auto line = [&](int x0, int y0, int x1, int y1) {
		const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), stepX = x0 < x1 ? 1 : -1, stepY = y0 < y1 ? 1 : -1;
		int error = dx + dy;
		while (true)
		{
			dot(x0, y0);
			if (x0 == x1 && y0 == y1) break;
			const int doubledError = 2 * error;
			if (doubledError >= dy) { error += dy; x0 += stepX; }
			if (doubledError <= dx) { error += dx; y0 += stepY; }
		}
	};

	// A dotted circle: a point every 4 degrees, of which only every n-th one is kept.
	auto circle = [&](int radius, int every) {
		int dotIndex = 0;
		for (int degrees = 0; degrees < 360; degrees += 4)
		{
			if (dotIndex++ % every) continue;
			dot((int)std::lround(radius * std::cos(degrees * Pi / 180.0)), (int)std::lround(radius * std::sin(degrees * Pi / 180.0)));
		}
	};

	// The shape of each fire mode, in pixels from the centre (y down).
	switch (mode)
	{
	case BA_SNAPSHOT:
		// An open cross with a centre dot.
		line(-9, 0, -4, 0); line(4, 0, 9, 0); line(0, -9, 0, -4); line(0, 4, 0, 9);
		dot(0, 0);
		break;
	case BA_AIMEDSHOT:
		// A scope: a ring with longer cross hairs through it.
		circle(9, 1);
		line(-12, 0, -5, 0); line(5, 0, 12, 0); line(0, -12, 0, -5); line(0, 5, 0, 12);
		dot(0, 0);
		break;
	case BA_AUTOSHOT:
	{
		// Brackets left and right, a small cross, and under it a 2x2 dot per round of the
		// burst, centred.
		line(-10, -5, -10, 5); line(-10, -5, -8, -5); line(-10, 5, -8, 5);
		line(10, -5, 10, 5); line(10, -5, 8, -5); line(10, 5, 8, 5);
		line(-3, 0, 3, 0); line(0, -3, 0, 3);
		const int rounds = std::clamp(shots, 1, 6);
		for (int roundIndex = 0; roundIndex < rounds; ++roundIndex)
		{
			const int x = roundIndex * 4 - (rounds - 1) * 2;
			dot(x, 9); dot(x + 1, 9); dot(x, 10); dot(x + 1, 10);
		}
		break;
	}
	case BA_THROW:
		// A dotted arc (a parabola) coming down onto a small landing ring.
		for (int x = -12; x <= 12; x += 3)
		{
			dot(x, -8 + (x * x) / 18);
		}
		circle(4, 1);
		dot(0, 0);
		break;
	case BA_LAUNCH:
		// A diamond with a small cross.
		line(0, -10, 10, 0); line(10, 0, 0, 10); line(0, 10, -10, 0); line(-10, 0, 0, -10);
		line(-2, 0, 2, 0); line(0, -2, 0, 2);
		break;
	case BA_HIT:
		// Three slanted claw marks.
		line(-9, -6, -5, 6); line(-2, -8, 2, 8); line(5, -6, 9, 6);
		break;
	case BA_PANIC:
	case BA_MINDCONTROL:
		// Psi: three dotted rings spreading out.
		circle(4, 2); circle(9, 2); circle(14, 3);
		dot(0, 0);
		break;
	case BA_USE:
	case BA_PRIME:
		// An item used from its menu: a box.
		line(-5, -5, 5, -5); line(5, -5, 5, 5); line(5, 5, -5, 5); line(-5, 5, -5, -5);
		dot(0, 0);
		break;
	default:
		// Nothing to fire: a small plus.
		dot(0, 0); dot(-1, 0); dot(1, 0); dot(0, -1); dot(0, 1);
		break;
	}

	// The hitmarker: four ticks on the diagonals (white for a hit; for a kill red, and longer).
	// The points from here on get the hit colour.
	const size_t modePoints = points.size();
	if (hitMark)
	{
		const int tickInner = hitMark == 2 ? 6 : 7, tickOuter = hitMark == 2 ? 12 : 10;
		line(-tickInner, -tickInner, -tickOuter, -tickOuter); line(tickInner, -tickInner, tickOuter, -tickOuter);
		line(-tickInner, tickInner, -tickOuter, tickOuter); line(tickInner, tickInner, tickOuter, tickOuter);
	}

	// Draw: first a dark 3x3 block behind every point (the outline), then the points in colour.
	_crosshair->clear();
	for (const auto &point : points)
	{
		for (int offsetY = -1; offsetY <= 1; ++offsetY)
			for (int offsetX = -1; offsetX <= 1; ++offsetX)
				_crosshair->setPixel(point.first + offsetX, point.second + offsetY, shadow);
	}
	for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
	{
		_crosshair->setPixel(points[pointIndex].first, points[pointIndex].second, pointIndex < modePoints ? color : hitColor);
	}
}

/**
 * Updates the crosshair and the two lines under it, every frame: the mode's name (with the
 * burst size), then the hit chance at what's under the crosshair and the time unit cost.
 * The hit chance is worked out as the game's own isometric cursor does it (the game's firing
 * accuracy, then its range dropoff and maximum range; only the cursor's penalty for a target
 * out of the soldier's line of sight is left out), so the two agree. The
 * crosshair is yellow when the soldier can do it, red when he can't (time units, ammo, range),
 * purple for psi, and grey when it's not his move. In FPS style it shows the spread as a
 * dotted ring instead of odds.
 * @param unit The soldier being looked through (may be null).
 * @param playerTurn It's the player's turn (otherwise only a plain crosshair).
 */
void FirstPersonMode::updateCrosshair(BattleUnit *unit, bool playerTurn)
{
	// Shown only in the 3D view, with the crosshair setting; the lines with the "Shot info"
	// setting, in the player's turn.
	const bool show = Options::fpsCrosshair && isActive();
	_crosshair->setVisible(show);
	_txtMode->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	_txtShot->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	if (!show)
	{
		return;
	}

	// The colours, picked from the current palette: plain ones for the crosshair, and text
	// colour groups for the lines.
	const SDL_Color *palette = _view->getPalette();
	const Uint8 yellow = nearestColor(palette, 255, 216, 64), red = nearestColor(palette, 232, 56, 40);
	const Uint8 grey = nearestColor(palette, 150, 150, 150), shadow = nearestColor(palette, 8, 8, 8), psi = nearestColor(palette, 200, 120, 255);
	const Uint8 yellowText = textColor(palette, 255, 216, 64), redText = textColor(palette, 240, 70, 50);
	const Uint8 greyText = textColor(palette, 170, 170, 170), psiText = textColor(palette, 200, 120, 255), greenText = textColor(palette, 130, 230, 100);

	// The weapon: the one in hand, or the item of an action picked from a menu that's being
	// aimed now.
	BattleItem *weapon = (playerTurn && unit) ? activeWeapon(unit) : nullptr;
	if (playerTurn && unit)
	{
		const BattleAction *action = _state->_battleGame->getCurrentAction();
		if (action->targeting && action->actor == unit && action->weapon)
		{
			weapon = action->weapon;
		}
	}
	const BattleActionType slot = (playerTurn && weapon) ? _fireMode : BA_NONE;
	const BattleActionType mode = firedAs(weapon, slot); // (FPS style: the quick slot is a burst)
	int shots = 1;
	Uint8 color = grey;
	std::string modeText, shotText;
	bool dual = false;
	if (weapon && mode != BA_NONE && unit)
	{
		const RuleItem *rules = weapon->getRules();
		if (mode == BA_AUTOSHOT) shots = rules->getConfigAuto()->shots;

		// Can he do it: the game's own cost in time units (Doom mode never runs out), and for
		// a shot, ammo loaded if the weapon needs any.
		const BattleActionCost cost(mode, unit, weapon);
		bool canFire = Options::fpsDoomMode || cost.Time <= unit->getTimeUnits();
		if (isShot(mode) || mode == BA_LAUNCH)
		{
			canFire = canFire && (!weapon->needsAmmoForAction(mode) || weapon->getAmmoForAction(mode));
		}

		// Dual wield: the other hand's gun joins in if there's time for both (offTime -1: there
		// isn't, so the chosen gun fires alone, as fire() decides too).
		BattleActionType offMode = BA_NONE;
		int offTime = 0;
		if (BattleItem *otherWeapon = canFire ? dualPartner(unit, weapon, slot, offMode) : nullptr)
		{
			offTime = BattleActionCost(offMode, unit, otherWeapon).Time;
			if (!Options::fpsDoomMode && cost.Time + offTime > unit->getTimeUnits()) offTime = -1;
		}
		dual = offTime > 0 || (offTime == 0 && offMode != BA_NONE);

		// The hit chance at what's under the crosshair, for shots and throws (not in FPS style:
		// no odds there, the spread ring shows it). As the game's isometric cursor works it out
		// (Map.cpp): the soldier's firing accuracy with this weapon and mode, less the dropoff
		// per tile beyond the weapon's aim range or short of its minimum range (then shown
		// yellow); zero (red) beyond its maximum range.
		std::ostringstream shotInfo;
		int accuracy = -1;
		Uint8 accuracyColor = greenText;
		if ((isShot(mode) || mode == BA_THROW) && _aimHitsSomething && !freeAimOn(mode))
		{
			const BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(mode, unit, weapon);
			accuracy = BattleUnit::getFiringAccuracy(attack, _game->getMod());
			if (dual) accuracy /= 2; // (before the range dropoff, as the shot does)
			const int distanceSq = unit->distance3dToPositionSq(_aimVoxel.toTile());
			const int distance = (int)std::ceil(std::sqrt((double)distanceSq)); // in tiles
			int upperLimit, lowerLimit;
			const int dropoff = rules->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), mode);
			if (distance > upperLimit) { accuracy -= (distance - upperLimit) * dropoff; accuracyColor = yellowText; }
			else if (distance < lowerLimit) { accuracy -= (lowerLimit - distance) * dropoff; accuracyColor = yellowText; }
			const bool outOfRange = mode == BA_THROW ? rules->isOutOfThrowRange(distanceSq, _save->getDepth()) : rules->isOutOfRange(distanceSq);
			if (accuracy <= 0 || outOfRange)
			{
				accuracy = 0;
				accuracyColor = redText;
				canFire = false;
			}
			shotInfo << accuracy << "%  ";
		}

		// Then the time units it costs (both guns' with dual wield).
		shotInfo << cost.Time + (dual ? offTime : 0) << std::string(_state->tr("STR_FPS_TU"));
		shotText = shotInfo.str();

		// The mode's name. FPS style: the quick slot reads "BURST x<rounds>", and auto says to
		// hold the trigger; otherwise auto shows its number of rounds. "DUAL" goes in front
		// with dual wield.
		static const struct { BattleActionType mode; const char *name; } modeNames[] = {
			{ BA_SNAPSHOT, "STR_FPS_MODE_SNAP" }, { BA_AIMEDSHOT, "STR_FPS_MODE_AIMED" }, { BA_AUTOSHOT, "STR_FPS_MODE_AUTO" },
			{ BA_THROW, "STR_FPS_MODE_THROW" }, { BA_LAUNCH, "STR_FPS_MODE_LAUNCH" }, { BA_HIT, "STR_FPS_MODE_MELEE" },
			{ BA_PANIC, "STR_FPS_MODE_PANIC" }, { BA_MINDCONTROL, "STR_FPS_MODE_MIND_CONTROL" }, { BA_USE, "STR_FPS_MODE_USE" },
			{ BA_PRIME, "STR_FPS_MODE_PRIME" } };
		for (const auto &entry : modeNames)
		{
			if (entry.mode == mode) modeText = _state->tr(entry.name);
		}
		if (mode == BA_AUTOSHOT && slot != mode)
		{
			modeText = std::string(_state->tr("STR_FPS_MODE_BURST")) + " x" + std::to_string(shots); // FPS style quick slot
		}
		else if (mode == BA_AUTOSHOT)
		{
			modeText += freeAimOn(mode) ? std::string(" ") + std::string(_state->tr("STR_FPS_HOLD")) : " x" + std::to_string(shots);
		}
		if (dual)
		{
			modeText = std::string(_state->tr("STR_FPS_DUAL")) + " " + modeText;
		}

		// Colours: red if he can't, purple for psi, yellow otherwise; all grey while he can't
		// act (it's not his move, the game is busy). The hit chance keeps its own colour
		// (green, yellow with dropoff) while the shot can be taken.
		color = !canFire ? red : (mode == BA_PANIC || mode == BA_MINDCONTROL) ? psi : yellow;
		Uint8 labelColor = !canFire ? redText : (mode == BA_PANIC || mode == BA_MINDCONTROL) ? psiText : yellowText;
		if (!canAct())
		{
			color = grey;
			labelColor = greyText;
		}
		_txtMode->setColor(labelColor);
		_txtShot->setColor(accuracy >= 0 && canFire ? accuracyColor : labelColor);
	}
	_txtMode->setText(modeText);
	_txtShot->setText(shotText);

	// A hit (or a kill) on an enemy in sight ticks once and marks the crosshair for 280 ms.
	const Uint32 hitTick = _view->getHitMarkTick();
	if (hitTick != _hitMarkerHeard)
	{
		_hitMarkerHeard = hitTick;
		_sounds->play(_view->getHitMarkKill() ? FirstPersonSounds::KILL : FirstPersonSounds::HIT, 0, 0.7f);
	}
	const int hitMark = hitTick && SDL_GetTicks() - hitTick < 280 ? (_view->getHitMarkKill() ? 2 : 1) : 0;
	const Uint8 hitColor = hitMark == 2 ? red : nearestColor(palette, 255, 255, 255);

	// FPS style: a dotted ring shows the spread (it grows with the bloom of the last rounds).
	// Its radius in pixels is the spread angle seen through the view's lens, kept inside the
	// crosshair surface.
	int ringRadius = 0;
	if (unit && freeAimOn(mode))
	{
		const double focalPixels = (_view->getWidth() / 2.0) / std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0);
		ringRadius = std::clamp((int)std::lround(std::tan((spreadFor(unit, mode, dual) + _bloom) * Pi / 180.0) * focalPixels), 2, CrosshairSize / 2 - 1);
	}

	// Redraw the crosshair only when what it shows has changed: all of that packed into one
	// number (mode, rounds, colour, hitmarker, ring radius) and compared with the last one.
	const Uint32 crosshairKey = (Uint32)mode | ((Uint32)shots << 8) | ((Uint32)color << 16) | ((Uint32)hitMark << 24) | ((Uint32)ringRadius << 26);
	if (crosshairKey != _crosshairDrawnFor)
	{
		_crosshairDrawnFor = crosshairKey;
		drawCrosshair(mode, shots, color, shadow, hitMark, hitColor);

		// The spread ring: a dot every 12 degrees, each with a shadow down and to the right.
		if (ringRadius)
		{
			_crosshair->lock();
			const int centre = CrosshairSize / 2;
			for (int degrees = 0; degrees < 360; degrees += 12)
			{
				const int x = centre + (int)std::lround(ringRadius * std::cos(degrees * Pi / 180.0)),
					y = centre + (int)std::lround(ringRadius * std::sin(degrees * Pi / 180.0));
				_crosshair->setPixel(x + 1, y + 1, shadow);
				_crosshair->setPixel(x, y, color);
			}
			_crosshair->unlock();
		}
	}
}

/**
 * Tosses a tactical flare at the crosshair (the flare key, T by default, with the setting
 * "Unlimited tactical flares"). It's the add-on's own: free, unlimited, and no item or time units are used. The 3D
 * view flies it, bounces it off walls and lets it burn with its light.
 */
void FirstPersonMode::throwFlare()
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!Options::fpsFlares || !unit || !isActive() || _deathUnit)
	{
		return;
	}

	// Towards what's under the crosshair (a point far along the ray if nothing's hit).
	Position aimedVoxel;
	aimAt(_view->getWidth() / 2, _view->getHeight() / 2, unit, aimedVoxel);
	const float flareTarget[3] = { (float)aimedVoxel.x, (float)aimedVoxel.y, (float)aimedVoxel.z };
	_view->throwFlare(flareTarget);

	// The game's own throwing sound, from the soldier's tile, and our strike and hiss on top.
	if (Sound *sound = _game->getMod()->getSoundByDepth(_save->getDepth(), Mod::ITEM_THROW))
	{
		sound->play(-1, _state->_map->getSoundAngle(unit->getPosition()));
	}
	_sounds->play(FirstPersonSounds::FLARE, 0, 0.55f);
}

/**
 * Aims an action on the isometric map: a guided missile's waypoints, or a psi attack or mind
 * probe on a creature out of sight. The view switches to the map with the game's action already
 * being aimed, and from there the game's own clicks (and its launch button) take over; once the
 * action is done or called off, the view comes back to 3D (see updateMapAim).
 * @param unit The soldier acting.
 * @param weapon The launcher, psi amp or mind probe.
 * @param mode The action (BA_LAUNCH, BA_PANIC, BA_MINDCONTROL or BA_USE).
 * @param hint Translation key of the message shown on the map (what to do there).
 */
void FirstPersonMode::startMapAim(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const char *hint)
{
	// Set up the game's current action as its own menu would, with the targeting cursor.
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();
	action->actor = unit;
	action->weapon = weapon;
	action->type = mode;
	action->targeting = true;
	action->waypoints.clear();
	action->updateTU();
	battleGame->setupCursor();

	// Remember what's being aimed, switch to the isometric map at once, and say what to do.
	_mapAimPhase = MAPAIM_AIMING;
	_mapAimType = mode;
	_wants3D = false;
	rememberChoices();
	think();
	_state->warning(hint);
}

/**
 * Follows an action being aimed on the isometric map, every frame, and switches back to 3D
 * when it's done or called off. Aiming ends when the game gets busy (the action happens:
 * the missile flies, the psi attack goes off) or the action is no longer being aimed (done at
 * once, or called off); the action is over when the game is idle again with no message up.
 */
void FirstPersonMode::updateMapAim()
{
	if (_mapAimPhase == MAPAIM_NONE)
	{
		return;
	}
	const BattleAction *action = _state->_battleGame->getCurrentAction();
	const bool busy = _state->_battleGame->isBusy();
	bool done = false;
	if (_mapAimPhase == MAPAIM_AIMING)
	{
		// Still aiming: watch for it being fired or dropped.
		if (busy)
		{
			_mapAimPhase = MAPAIM_ACTING; // fired: the missile flies, the psi attack happens
		}
		else if (!(action->targeting && action->type == _mapAimType))
		{
			done = true; // done at once (the mind probe's report) or called off
		}
	}
	else if (!busy && _state->_popups.empty())
	{
		// Acting: done once the game is idle and its messages (psi results...) are closed.
		done = true;
	}

	// Done: back to 3D (if the player hasn't switched back already).
	if (done)
	{
		_mapAimPhase = MAPAIM_NONE;
		if (!_wants3D)
		{
			_wants3D = true;
			rememberChoices();
		}
	}
}

/**
 * Tells whether there's a creature the player can see (other than the soldier himself) at a
 * point. The point's own tile is checked, and the tile below it: a unit belongs to the tile it
 * stands on, but on raised ground its head can reach up into the level above.
 * @param voxel The point, in voxels.
 * @param self The soldier, who doesn't count.
 * @return True if a visible, conscious unit is there.
 */
bool FirstPersonMode::creatureAt(const Position &voxel, const BattleUnit *self) const
{
	for (int levelOffset = 0; levelOffset >= -1; --levelOffset)
	{
		const Tile *tile = _save->getTile(voxel.toTile() + Position(0, 0, levelOffset));
		const BattleUnit *creature = tile ? tile->getUnit() : nullptr;
		if (creature && creature != self && !creature->isOut() && creature->getVisible())
		{
			return true;
		}
	}
	return false;
}

/**
 * FPS-style auto, every frame: holding the trigger (left mouse button, in mouse look) fires the
 * next burst as soon as the game is done with the last one, while the clip and the time units
 * last. Each burst is an ordinary fire() through the game's rules.
 * @param unit The soldier being looked through.
 * @param playerTurn It's the player's turn.
 */
void FirstPersonMode::keepFiringWhileHeld(BattleUnit *unit, bool playerTurn)
{
	// The button may have been let go while another screen had the mouse.
	if (_triggerHeld && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)))
	{
		_triggerHeld = false;
	}

	// Only for FPS-style auto, with mouse look, when the soldier may act and the game is idle.
	if (_triggerHeld && playerTurn && _fireMode == BA_AUTOSHOT && freeAimOn(_fireMode) && _mouseLook && canAct()
		&& !_state->_battleGame->isBusy() && unit->getFaction() == FACTION_PLAYER)
	{
		// Another burst if there's ammo and time for it (by the game's cost); otherwise the
		// trigger counts as let go.
		BattleItem *weapon = activeWeapon(unit);
		BattleActionCost cost(BA_AUTOSHOT, unit, weapon);
		if (weapon && weapon->getAmmoForAction(BA_AUTOSHOT) && (Options::fpsDoomMode || cost.haveTU()))
		{
			fire(_view->getWidth() / 2, _view->getHeight() / 2);
		}
		else
		{
			_triggerHeld = false; // empty, or out of time
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
