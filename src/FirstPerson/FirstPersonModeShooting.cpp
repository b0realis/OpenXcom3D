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
#include <functional>
#include <sstream>
#include <string>
#include "FirstPersonSounds.h"
#include "FirstPersonApi.h"
#include "FirstPersonView.h"
#include "../Battlescape/AlienInventoryState.h"
#include "../Battlescape/BattleState.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/ExplosionBState.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/MedikitState.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/ProjectileFlyBState.h"
#include "../Battlescape/ScannerState.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitInfoState.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/InteractiveSurface.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Engine/Sound.h"
#include "../Engine/State.h"
#include "../Interface/Frame.h"
#include "../Interface/Text.h"
#include "../Mod/RuleInterface.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

namespace
{

/**
 * Holds the battle while a Doom free grenade flies (see FirstPersonMode::throwFreeGrenade), as
 * the game's own flight state does for its throws: every tick it flies the grenade on (step,
 * handed the time) and takes itself off the queue once the flight is over.
 */
class FreeGrenadeFlight : public BattleState
{
	std::function<bool(Uint32)> _step; // flies the grenade on to a time; false once it's over
public:
	/// Starts holding the battle run by `parent`, flying the grenade with `step`.
	FreeGrenadeFlight(BattlescapeGame *parent, std::function<bool(Uint32)> step) : BattleState(parent), _step(std::move(step)) { }
	/// Each tick: on with the flight, or off the queue once it's over.
	void think() override
	{
		if (!_step(SDL_GetTicks())) _parent->popState();
	}
};

/**
 * Doom real-time grenades: the fuse picked in seconds, 1 to FirstPerson::DoomFuseMaxSeconds, the
 * way the isometric map picks a grenade's timer (PrimeGrenadeState): a small box made like the
 * game's own (its frame, colours and big numbers, "SET FUSE" over 1s to 5s). A click on a number,
 * or its key (1 to 5), picks it; a right click or the cancel key changes nothing. What the pick
 * does is up to whoever opened it (see FirstPersonMode::fire).
 */
class DoomFuseState : public State
{
public:
	/**
	 * Makes the box, centred like the game's prime menu.
	 * @param onPicked Called with the seconds picked, after the box has gone.
	 */
	explicit DoomFuseState(std::function<void(int)> onPicked) : _onPicked(std::move(onPicked))
	{
		_screen = false;
		const int count = FirstPerson::DoomFuseMaxSeconds;
		const int rowWidth = count * 24 - 2, boxWidth = 128, boxX = (320 - boxWidth) / 2, firstX = boxX + (boxWidth - rowWidth) / 2;
		_frame = new Frame(boxWidth, 27, boxX, 70);
		_title = new Text(boxWidth, 24, boxX, 77);
		_background = new Surface(boxWidth, 44, boxX, 78);
		for (int seconds = 0; seconds < count; ++seconds)
		{
			_button[seconds] = new InteractiveSurface(22, 22, firstX - 1 + seconds * 24, 98);
			_number[seconds] = new Text(20, 20, firstX + seconds * 24, 99);
		}
		_game->getSavedGame()->getSavedBattle()->setPaletteByDepth(this);

		// The game's own look: the grenade menu's background, frame and text colours.
		const Element *colors = _game->getMod()->getInterface("battlescape")->getElement("grenadeBackground");
		add(_background);
		_background->drawRect(0, 0, _background->getWidth(), _background->getHeight(), colors->color);
		add(_frame, "grenadeMenu", "battlescape");
		_frame->setThickness(3);
		_frame->setHighContrast(true);
		add(_title, "grenadeMenu", "battlescape");
		_title->setAlign(ALIGN_CENTER);
		_title->setBig();
		_title->setText(tr("STR_FPS_DOOM_SET_FUSE"));
		_title->setHighContrast(true);
		for (int seconds = 0; seconds < count; ++seconds)
		{
			add(_button[seconds]);
			_button[seconds]->onMouseClick((ActionHandler)&DoomFuseState::buttonClicked);
			_button[seconds]->onKeyboardPress((ActionHandler)&DoomFuseState::buttonClicked, (SDLKey)(SDLK_1 + seconds));
			SDL_Rect square = { 0, 0, (Uint16)_button[seconds]->getWidth(), (Uint16)_button[seconds]->getHeight() };
			_button[seconds]->drawRect(&square, colors->border);
			square.x++; square.y++; square.w -= 2; square.h -= 2;
			_button[seconds]->drawRect(&square, colors->color2);
			add(_number[seconds], "grenadeMenu", "battlescape");
			_number[seconds]->setBig();
			_number[seconds]->setText(std::to_string(seconds + 1) + "s");
			_number[seconds]->setHighContrast(true);
			_number[seconds]->setAlign(ALIGN_CENTER);
			_number[seconds]->setVerticalAlign(ALIGN_MIDDLE);
		}
		centerAllSurfaces();
		lowerAllSurfaces();
	}

	/// A right click or the cancel key: closed, nothing picked.
	void handle(Action *action) override
	{
		State::handle(action);
		const SDL_Event &event = *action->getDetails();
		if ((event.type == SDL_MOUSEBUTTONDOWN && _game->isRightClick(action)) || (event.type == SDL_KEYDOWN && event.key.keysym.sym == Options::keyCancel))
		{
			_game->popState();
		}
	}

private:
	Frame *_frame;
	Text *_title;
	Surface *_background;
	InteractiveSurface *_button[FirstPerson::DoomFuseMaxSeconds];
	Text *_number[FirstPerson::DoomFuseMaxSeconds];
	std::function<void(int)> _onPicked;

	/// A number clicked (or its key pressed): the box goes, then the pick is handed on.
	void buttonClicked(Action *action)
	{
		if (_game->isRightClick(action)) return; // (handle closes the box)
		for (int seconds = 0; seconds < FirstPerson::DoomFuseMaxSeconds; ++seconds)
		{
			if (action->getSender() != _button[seconds]) continue;
			const std::function<void(int)> onPicked = _onPicked; // (the box is deleted later, but don't lean on it)
			_game->popState();
			onPicked(seconds + 1);
			return;
		}
	}
};

}

/**
 * Finds the weapon the soldier fires with: what the active hand gives (see activeHand).
 * @param unit The soldier.
 * @return The weapon, or null if there's none.
 */
BattleItem *FirstPersonMode::activeWeapon(BattleUnit *unit) const
{
	const int hand = activeHand(unit);
	return hand < 0 ? nullptr : handWeapon(unit, hand);
}

/**
 * The hand the soldier fires with. Every soldier starts with the right hand; G (or a pick on
 * the classic HUD) switches between the right hand, the left and the built-in weapon (the one
 * the classic HUD's SPECIAL button uses). A hand holds an item, or, empty, it gives the unit's
 * empty-hand weapon if it has one (fists, a claw), but only when the player picked that hand:
 * otherwise, as before, an empty hand leaves it to the other hand's item. When the hand
 * picked gives nothing any more (its item thrown, say), the right hand's item, the left's, the
 * empty-hand weapon and the built-in weapon are tried in turn.
 * @param unit The soldier.
 * @return 0 right, 1 left, 2 the built-in weapon, or -1 if there's nothing to use.
 */
int FirstPersonMode::activeHand(BattleUnit *unit) const
{
	auto handEntry = _chosenHand.find(unit);
	const bool picked = handEntry != _chosenHand.end();
	const int chosen = picked ? handEntry->second : 0;
	BattleType specialType;
	if (chosen == 2 ? unit->getSpecialIconWeapon(specialType) != nullptr
		: ((chosen ? unit->getLeftHandWeapon() : unit->getRightHandWeapon()) || (picked && emptyHandWeapon(unit))))
	{
		return chosen;
	}
	if (unit->getRightHandWeapon()) return 0;
	if (unit->getLeftHandWeapon()) return 1;
	if (emptyHandWeapon(unit)) return chosen == 1 ? 1 : 0;
	if (unit->getSpecialIconWeapon(specialType)) return 2;
	return -1;
}

/**
 * What a hand gives the soldier to use: the item it holds, or, empty, the unit's empty-hand
 * weapon (if any); the built-in weapon for hand 2.
 * @param unit The soldier.
 * @param hand 0 right, 1 left, 2 the built-in weapon.
 * @return The weapon, or null.
 */
BattleItem *FirstPersonMode::handWeapon(BattleUnit *unit, int hand) const
{
	if (hand == 2)
	{
		BattleType specialType;
		return unit->getSpecialIconWeapon(specialType);
	}
	BattleItem *held = hand ? unit->getLeftHandWeapon() : unit->getRightHandWeapon();
	return held ? held : emptyHandWeapon(unit);
}

/**
 * The unit's empty-hand weapon: a special weapon of its armor or kind that an empty hand uses
 * (OXCE's specialUseEmptyHand: fists, a claw), looked for as the game's hand buttons look for
 * it, by kind in the same order.
 * @param unit The unit.
 * @return The weapon, or null if it has none.
 */
BattleItem *FirstPersonMode::emptyHandWeapon(BattleUnit *unit) const
{
	for (const BattleType type : { BT_MELEE, BT_PSIAMP, BT_FIREARM, BT_MEDIKIT, BT_SCANNER, BT_MINDPROBE })
	{
		BattleItem *weapon = unit->getSpecialWeapon(type);
		if (weapon && weapon->getRules()->isSpecialUsingEmptyHand())
		{
			return weapon;
		}
	}
	return nullptr;
}

/**
 * The hand a weapon is used from: the hand holding it, the built-in weapon's, or for the
 * empty-hand weapon the hand it's used from now (an empty one: the left if it's empty, else
 * the right).
 * @param unit The soldier.
 * @param weapon The weapon.
 * @return 0 right, 1 left, 2 the built-in weapon.
 */
int FirstPersonMode::handOf(BattleUnit *unit, const BattleItem *weapon) const
{
	if (weapon && weapon == unit->getLeftHandWeapon()) return 1;
	if (weapon && weapon == unit->getRightHandWeapon()) return 0;
	BattleType specialType;
	if (weapon && weapon == unit->getSpecialIconWeapon(specialType) && weapon != emptyHandWeapon(unit)) return 2;
	const int active = activeHand(unit);
	if (weapon && weapon == emptyHandWeapon(unit) && (active == 0 || active == 1) && handWeapon(unit, active) == weapon) return active;
	return unit->getLeftHandWeapon() ? 0 : 1;
}

/**
 * The hand's letter in front of an item's name on the panel and the squad card: R or L for
 * the hands (an empty hand's weapon by the hand it's used from), S for the built-in (special)
 * weapon.
 * @param unit The soldier.
 * @param item The item.
 * @return "R: ", "L: " or "S: ".
 */
std::string FirstPersonMode::handLetter(BattleUnit *unit, const BattleItem *item) const
{
	const int hand = handOf(unit, item);
	return hand == 2 ? "S: " : hand == 1 ? "L: " : "R: ";
}

/**
 * Melee at the downed (setting "Melee hits walls and the downed"): with no one standing in
 * reach, the blow may hit a unit lying unconscious in front of the soldier (in the tile they
 * face, if no wall is in between) or at their feet. The game's melee wants a standing target,
 * so the blow is the game's own pieces put together here: the time units and ammo the game's
 * melee spends, the weapon's melee power and damage type as the game works them out, a hit (the
 * body can't dodge) through the game's own unit damage (TileEngine::hitUnit, as a shot at the
 * body would), the weapon's melee sound, the game's check for casualties, and reaction fire.
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @param action The game's current action, to fill in.
 * @return True if there was a body to hit (the blow was made, or refused with the reason
 *         shown); false if there's none in reach.
 */
bool FirstPersonMode::meleeDowned(BattleUnit *unit, BattleItem *weapon, BattleAction *action)
{
	// The body: in the tile the soldier faces (on its floor, or on the top of stairs below it,
	// as the game's melee looks), with no wall in between, or else at their feet.
	Position ahead;
	Pathfinding::directionToVector(unit->getDirection(), &ahead);
	Tile *ownTile = _save->getTile(unit->getPosition());
	Tile *aheadTile = _save->getTile(unit->getPosition() + ahead);
	if (aheadTile && aheadTile->hasNoFloor(_save) && _save->getBelowTile(aheadTile) && _save->getBelowTile(aheadTile)->getTerrainLevel() <= -16)
	{
		aheadTile = _save->getBelowTile(aheadTile);
	}
	if (ownTile && aheadTile && _save->getPathfinding()->isBlockedDirection(unit, ownTile, unit->getDirection()))
	{
		aheadTile = nullptr;
	}
	BattleUnit *body = nullptr;
	for (Tile *tile : { aheadTile, ownTile })
	{
		for (BattleItem *item : tile ? *tile->getInventory() : std::vector<BattleItem*>())
		{
			if (!body && item->getUnit() && item->getUnit() != unit && item->getUnit()->getStatus() == STATUS_UNCONSCIOUS)
			{
				body = item->getUnit();
			}
		}
	}
	if (!body)
	{
		return false;
	}

	// The cost: the game's time units for the blow and the weapon's ammo for it.
	action->type = BA_HIT;
	action->targeting = false;
	action->target = body->getPosition();
	action->result.clear();
	action->updateTU();
	BattleItem *ammo = weapon->getAmmoForAction(BA_HIT, &action->result);
	if (!ammo || !action->spendTU(&action->result))
	{
		_state->warning(action->result.empty() ? "STR_NOT_ENOUGH_TIME_UNITS" : action->result);
		action->type = BA_NONE;
		return true;
	}
	weapon->spendAmmoForAction(BA_HIT, _save);

	// The blow's power and damage type, as the game works them out for a melee hit: a melee
	// weapon's own power and damage, or a gun's melee bonus and melee damage (a rifle butt).
	const BattleActionAttack attack = BattleActionAttack::GetAferShoot(*action, ammo);
	const RuleItem *damageRule = attack.damage_item->getRules();
	const bool meleeWeapon = damageRule->getBattleType() == BT_MELEE;
	const int power = meleeWeapon ? damageRule->getPowerBonus(attack) : damageRule->getMeleeBonus(attack);
	const RuleDamageType *damageType = meleeWeapon ? damageRule->getDamageType() : damageRule->getMeleeType();

	// The hit, its sound, the casualties, and any reaction fire the time spent draws.
	int sound = weapon->getRules()->getMeleeHitSound();
	if (sound < 0) sound = weapon->getRules()->getMeleeSound();
	if (sound >= 0) _state->_battleGame->playSound(sound);
	_save->getTileEngine()->hitUnit(attack, body, Position(0, 0, 0), damageType->getRandomDamage(power), damageType, false);
	_state->_battleGame->checkForCasualties(damageType, attack, false, false);
	_save->getTileEngine()->checkReactionFire(unit, *action);
	action->type = BA_NONE;

	// The weapon in hand swings, as for any blow that lands.
	_swingStart = SDL_GetTicks();
	_swingJolted = false;
	if (Options::fpsHitSounds) _sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f); // (one of our added sounds: setting "Hit, kill and swing sounds")
	return true;
}

/**
 * Akimbo (dual wield, a setting): finds the gun in the other hand, which is drawn on its own
 * side of the view and fires together with the chosen one.
 * Both weapons must be firearms; a guided launcher (one with waypoints, like the blaster
 * launcher) doesn't count, since its missile is steered on the isometric map. With "Full
 * akimbo" any two guns pair up (a two-handed one takes the game's one-handed penalty); with
 * "Realistic akimbo" only guns that aren't two-handed do.
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @return The other hand's gun, or null if akimbo is off or the two don't pair up.
 */
BattleItem *FirstPersonMode::offHandWeapon(BattleUnit *unit, const BattleItem *weapon) const
{
	// The chosen weapon itself must be a plain gun, and akimbo must be on.
	const bool akimbo = Options::fpsDualWield || Options::fpsAkimboRealistic;
	if (!akimbo || !unit || !weapon || weapon->getRules()->getBattleType() != BT_FIREARM || weapon->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}

	// The same test for whatever is in the other hand (and it must be a second item). A weapon
	// not held in a hand (an empty hand's fists, the built-in weapon) fires alone.
	if (weapon != unit->getRightHandWeapon() && weapon != unit->getLeftHandWeapon())
	{
		return nullptr;
	}
	BattleItem *otherWeapon = weapon == unit->getRightHandWeapon() ? unit->getLeftHandWeapon() : unit->getRightHandWeapon();
	if (!otherWeapon || otherWeapon == weapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM || otherWeapon->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}

	// Realistic akimbo: a two-handed gun (a rifle, a rocket launcher) fires alone.
	if (Options::fpsAkimboRealistic && (weapon->getRules()->isTwoHanded() || otherWeapon->getRules()->isTwoHanded()))
	{
		return nullptr;
	}
	return otherWeapon;
}

/**
 * The akimbo key (Y): with full or realistic akimbo on, switches it off and remembers which it
 * was; with both off, switches that one back on (realistic if there's none to remember). The
 * game's warning line says which.
 */
void FirstPersonMode::toggleAkimbo()
{
	if (Options::fpsDualWield || Options::fpsAkimboRealistic)
	{
		_akimboRealisticWasOn = Options::fpsAkimboRealistic;
		Options::fpsDualWield = Options::fpsAkimboRealistic = false;
		_state->warning("STR_FPS_AKIMBO_OFF");
	}
	else
	{
		Options::fpsAkimboRealistic = _akimboRealisticWasOn;
		Options::fpsDualWield = !_akimboRealisticWasOn;
		_state->warning(_akimboRealisticWasOn ? "STR_FPS_AKIMBO_REALISTIC_ON" : "STR_FPS_AKIMBO_FULL_ON");
	}
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

	// Anything not built in can be thrown. Anything with a fuse (a grenade) not yet primed can
	// be primed; and once primed, unprimed again where its rules allow, as the game's action
	// menu has it (a mod's flashlight is switched on by priming, and off this way).
	if (!rules->isFixed() && rules->getCostThrow().Time > 0) modes.push_back(BA_THROW);
	if (!psiBlocked && rules->getFuseTimerType() != BFT_NONE)
	{
		if (weapon->getFuseTimer() == -1 && rules->getCostPrime().Time > 0) modes.push_back(BA_PRIME);
		if (weapon->getFuseTimer() >= 0 && rules->getCostUnprime().Time > 0 && !rules->getUnprimeActionName().empty()) modes.push_back(BA_UNPRIME);
	}
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
		_chosenHand[unit] = handOf(unit, action->weapon);
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
 * Switches the soldier's weapon hand (G by default): to the next of the right hand, the left
 * and the built-in weapon that gives something else to use: an item held, an empty hand's
 * weapon (fists, a claw), the built-in weapon (the classic HUD's SPECIAL). The fire mode is
 * picked again for the new weapon on the next frame (see updateFireMode). It costs nothing: on
 * the classic HUD each of them can be used at any time too.
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
		const int current = std::max(0, activeHand(unit));
		const BattleItem *currentWeapon = activeWeapon(unit);
		for (int step = 1; step <= 2; ++step)
		{
			const int next = (current + step) % 3;
			BattleItem *nextWeapon = handWeapon(unit, next);
			if (nextWeapon && nextWeapon != currentWeapon)
			{
				_chosenHand[unit] = next;
				break;
			}
		}
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
 * What the game's turn before a shot or a throw at what's under the crosshair will cost (shown
 * on the compass). The game turns the soldier to face the target tile first (BattleUnit::
 * directionTo: the eighth it lies in), an eighth at a time the shorter way round, each eighth
 * costing the armor's turn cost (a tank's turret turns instead, for 1), as UnitTurnBState
 * charges it. Turning by the rules only: with free turning the soldier faces the target for free
 * before the shot (see fire). Nothing for melee (it strikes the tile ahead), psi (no turn) or
 * in Doom mode (time units don't run out).
 * @param unit The soldier aiming.
 * @param direction Set to the way the soldier (or its turret) will face for the shot.
 * @param affordable Set to whether the soldier has the time units for the turn and the shot.
 * @return The time units the turn will cost; 0 for no turn.
 */
int FirstPersonMode::shotTurnCost(BattleUnit *unit, int &direction, bool &affordable) const
{
	direction = -1;
	affordable = true;
	if (!Options::fpsTurnCost || doomModeOn() || !unit)
	{
		return 0;
	}

	// The weapon and mode the next click fires, as the crosshair works them out.
	BattleItem *weapon = activeWeapon(unit);
	const BattleAction *action = _state->_battleGame->getCurrentAction();
	if (action->targeting && action->actor == unit && action->weapon)
	{
		weapon = action->weapon;
	}
	const BattleActionType mode = weapon ? firedAs(weapon, _fireMode) : BA_NONE;
	if (!isShot(mode) && mode != BA_THROW)
	{
		return 0;
	}

	// The target tile as fire() takes it (kept inside the map), and the eighths to turn.
	Position targetTile = _aimVoxel.toTile();
	targetTile.x = std::clamp<int>(targetTile.x, 0, _save->getMapSizeX() - 1);
	targetTile.y = std::clamp<int>(targetTile.y, 0, _save->getMapSizeY() - 1);
	targetTile.z = std::clamp<int>(targetTile.z, 0, _save->getMapSizeZ() - 1);
	const bool turret = unit->getTurretType() != -1;
	direction = unit->directionTo(targetTile);
	const int facing = turret ? unit->getTurretDirection() : unit->getDirection();
	const int eighths = std::min((direction - facing + 8) % 8, (facing - direction + 8) % 8);
	const int cost = eighths * (turret ? 1 : unit->getTurnCost());
	affordable = cost + BattleActionCost(mode, unit, weapon).Time <= unit->getTimeUnits();
	return cost;
}

/**
 * Pulls a grenade's pin for Doom mode's real-time fuse (see fire, updateDoomFuses): the game's
 * own prime action with its fuse at 0, if it isn't primed yet (the time units Doom mode refills,
 * the item's own prime message and sound), our pin's click where the item has no sound of its
 * own, and the fuse: the setting "Doom grenade fuse", burning from a moment or not yet.
 * @param unit The soldier holding it.
 * @param grenade The grenade.
 * @param lit When its fuse starts burning (SDL ticks); 0: when it leaves the hand.
 */
void FirstPersonMode::pullDoomPin(BattleUnit *unit, BattleItem *grenade, Uint32 lit)
{
	if (grenade->getFuseTimer() < 0)
	{
		BattleAction *action = _state->_battleGame->getCurrentAction();
		action->actor = unit;
		action->weapon = grenade;
		action->type = BA_PRIME;
		action->value = 0;
		action->targeting = false;
		action->result.clear();
		action->updateTU();
		_state->_battleGame->handleNonTargetAction();
	}
	if (grenade->getFuseTimer() < 0)
	{
		return; // the game refused (it says why)
	}
	if (grenade->getRules()->getPrimeSoundRaw().empty())
	{
		_sounds->play(FirstPersonSounds::PIN);
	}
	_doomFuses[grenade->getId()] = { lit, (Uint32)std::clamp(Options::fpsDoomGrenadeFuse, 1, FirstPerson::DoomFuseMaxSeconds) * 1000 };
}

/**
 * Doom free grenades (setting "Doom free grenades with airburst"): throws a grenade at a point
 * by the 3D view's own throw, not the game's, so it goes wherever the crosshair points: under a
 * roof, over a wall, at a point in the air. It leaves the hand (its real-time fuse, if it has one,
 * starts burning) from just below the eye and flies a real arc that passes through the point,
 * taking FreeGrenadeSpeed to get there (within FreeGrenadeTimeMin and Max), and no further than
 * the soldier could throw it by the game's rules (their strength against its weight) nor than a
 * person can throw (FreeGrenadeMaxTiles); then on, until it meets something. A short battle state holds the battle meanwhile, as the game's
 * flight does (see stepFreeGrenade). It makes the game's throw sound, and enemies may react to
 * it as to any throw. Time units: Doom mode refills them.
 * @param unit The thrower.
 * @param grenade The grenade, in their hand.
 * @param aimedVoxel The point under the crosshair (far along the view for the sky).
 */
void FirstPersonMode::throwFreeGrenade(BattleUnit *unit, BattleItem *grenade, const Position &aimedVoxel)
{
	// From just below the eye, a little ahead of it.
	double eye[3];
	_view->getEye(eye);
	const double yaw = _view->getYawDegrees() * Pi / 180.0;
	const float start[3] = { (float)(eye[0] + std::cos(yaw) * 3.0), (float)(eye[1] + std::sin(yaw) * 3.0), (float)(eye[2] - 3.0) };

	// To the point, or as far towards it as the soldier can throw this: the game's own reach for
	// their strength against its weight, but never further than a person can throw (see
	// FreeGrenadeMaxTiles: about 40 m for the strongest, less for the weaker).
	float towards[3] = { aimedVoxel.x + 0.5f - start[0], aimedVoxel.y + 0.5f - start[1], aimedVoxel.z + 0.5f - start[2] };
	const float level = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1]);
	const int strength = unit->getBaseStats()->strength;
	const float humanReach = FreeGrenadeMaxTiles * 16.0f * std::min(1.0f, strength / FreeGrenadeFullStrength);
	const float reach = std::min(humanReach, (float)ProjectileFlyBState::getMaxThrowDistance(grenade->getTotalWeight(), strength, (int)-towards[2]) + 8.0f);
	if (level > reach)
	{
		for (float &axis : towards) axis *= reach / level;
	}

	// The arc through it: flying for a time that grows with the distance, rising enough at the
	// start that gravity brings it down there just then.
	const float distance = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1] + towards[2] * towards[2]);
	const float seconds = std::clamp(distance / FreeGrenadeSpeed, FreeGrenadeTimeMin, FreeGrenadeTimeMax);
	FreeGrenade &flight = _freeGrenade;
	flight.item = grenade;
	flight.thrower = unit;
	for (int axis = 0; axis < 3; ++axis)
	{
		flight.position[axis] = start[axis];
		flight.velocity[axis] = towards[axis] / seconds;
	}
	flight.velocity[2] += 0.5f * FreeGrenadeGravity * seconds;
	flight.lastTick = SDL_GetTicks();

	// Out of the hand, the game's throw sound, the view drawing it, and the battle held while
	// it flies; enemies may react to the throw.
	grenade->moveToOwner(nullptr);
	_game->getMod()->getSoundByDepth(_save->getDepth(), Mod::ITEM_THROW)->play(-1, _state->_map->getSoundAngle(unit->getPosition()));
	_view->setFreeGrenade(grenade, flight.position, flight.velocity);
	BattlescapeGame *battleGame = _state->_battleGame;
	battleGame->statePushBack(new FreeGrenadeFlight(battleGame, [this](Uint32 now) { return stepFreeGrenade(now); }));
	BattleAction throwAction;
	throwAction.type = BA_THROW;
	throwAction.actor = unit;
	throwAction.weapon = grenade;
	_save->getTileEngine()->checkReactionFire(unit, throwAction);
}

/**
 * Flies the Doom free grenade on to now (see throwFreeGrenade), called by its battle state:
 * - its real-time fuse ran out (Doom real-time grenades): it bursts where it is, in the air (the
 *   game's own explosion for a timed grenade there, which also takes the grenade away);
 * - else it moves on along its arc, a voxel at a time, until it meets something (the game's own
 *   voxel check, the thrower left out) or leaves the map by a side or the bottom: then it drops
 *   there as the game drops a thrown item (its landing sound; a grenade the game sets off on
 *   landing goes off), its fuse burning on. Above the map's top there's nothing to meet: it comes
 *   back down (one bursting up there bursts just under the top).
 * @param now The time (SDL ticks).
 * @return False once the flight is over (landed or burst).
 */
bool FirstPersonMode::stepFreeGrenade(Uint32 now)
{
	FreeGrenade &flight = _freeGrenade;
	BattleItem *grenade = flight.item;
	if (!grenade)
	{
		return false;
	}
	BattlescapeGame *battleGame = _state->_battleGame;
	auto finish = [&]()
	{
		flight.item = nullptr;
		_view->setFreeGrenade(nullptr, flight.position, flight.velocity);
		return false;
	};
	auto voxelAt = [](const float point[3]) { return Position((int)std::floor(point[0]), (int)std::floor(point[1]), (int)std::floor(point[2])); };

	// Burst in the air: the fuse has run out.
	const auto fuse = _doomFuses.find(grenade->getId());
	if (Options::fpsDoomRealTimeGrenades && fuse != _doomFuses.end() && fuse->second.lit && now - fuse->second.lit >= fuse->second.length)
	{
		_doomFuses.erase(fuse);
		Position burst = voxelAt(flight.position);
		burst.z = std::min<int>(burst.z, _save->getMapSizeZ() * 24 - 1); // (above the map: just under its top)
		battleGame->statePushNext(new ExplosionBState(battleGame, burst, BattleActionAttack::GetBeforeShoot(BA_TRIGGER_TIMED_GRENADE, flight.thrower, grenade)));
		return finish();
	}

	// On along the arc, a voxel (or less) at a time.
	const float seconds = std::min(0.05f, (now - flight.lastTick) / 1000.0f);
	flight.lastTick = now;
	const float speed = std::sqrt(flight.velocity[0] * flight.velocity[0] + flight.velocity[1] * flight.velocity[1] + flight.velocity[2] * flight.velocity[2]);
	const int steps = std::max(1, (int)std::ceil(speed * seconds));
	const float stepSeconds = seconds / steps;
	const Position mapSize(_save->getMapSizeX() * 16, _save->getMapSizeY() * 16, _save->getMapSizeZ() * 24);
	for (int step = 0; step < steps; ++step)
	{
		float next[3];
		flight.velocity[2] -= FreeGrenadeGravity * stepSeconds;
		for (int axis = 0; axis < 3; ++axis) next[axis] = flight.position[axis] + flight.velocity[axis] * stepSeconds;
		// Out through a side or the bottom of the map: it lands there. Above the map's top there's
		// nothing to meet (it comes back down).
		const Position voxel = voxelAt(next);
		const bool outside = voxel.x < 0 || voxel.y < 0 || voxel.z < 0 || voxel.x >= mapSize.x || voxel.y >= mapSize.y;
		if (outside || (voxel.z < mapSize.z && _save->getTileEngine()->voxelCheck(voxel, flight.thrower) != V_EMPTY))
		{
			// Landed: dropped on the tile it was last in the air over (falling to its floor),
			// as the game lands a thrown item.
			Position tile = voxelAt(flight.position).toTile();
			tile.x = std::clamp<int>(tile.x, 0, _save->getMapSizeX() - 1);
			tile.y = std::clamp<int>(tile.y, 0, _save->getMapSizeY() - 1);
			tile.z = std::clamp<int>(tile.z, 0, _save->getMapSizeZ() - 1);
			_game->getMod()->getSoundByDepth(_save->getDepth(), Mod::ITEM_DROP)->play(-1, _state->_map->getSoundAngle(tile));
			if (grenade->fuseThrowEvent())
			{
				_doomFuses.erase(grenade->getId());
				battleGame->statePushNext(new ExplosionBState(battleGame, voxelAt(flight.position), BattleActionAttack::GetBeforeShoot(BA_TRIGGER_TIMED_GRENADE, flight.thrower, grenade)));
			}
			else
			{
				battleGame->dropItem(tile, grenade);
			}
			return finish();
		}
		std::copy(next, next + 3, flight.position);
	}
	_view->setFreeGrenade(grenade, flight.position, flight.velocity);
	return true;
}

/**
 * Is an item a grenade on a timer, one the Doom real-time fuse can take: a grenade (the
 * battle type: grenades, smoke grenades, high explosive) whose fuse is set by its thrower or
 * fixed, not one that goes off on landing (instant) or with none. Proximity grenades are another
 * battle type: they stay the game's own.
 * @param item The item (may be null).
 * @return True if it is.
 */
bool FirstPersonMode::timedGrenade(const BattleItem *item)
{
	if (!item) return false;
	const RuleItem *rules = item->getRules();
	return rules->getBattleType() == BT_GRENADE && rules->getFuseTimerType() != BFT_INSTANT && rules->getFuseTimerType() != BFT_NONE;
}

/**
 * Shows the path the next throw or shot would take (setting "Show trajectory": throws and
 * arcing shots, or every shot), or the path to a target marked for confirm fire (whatever the
 * setting), as a dotted line in the world (FirstPersonView::drawTrajectory). It is worked out
 * as the game will work it out, only without the accuracy roll (no dice are thrown for it):
 * - a throw or an arcing shot: from the game's origin voxel (the hand), the game picks the
 *   point to aim at on the target tile (a unit's middle, an object, a wall, the floor; a throw
 *   always the floor), finds the lowest curve that lands on the tile (TileEngine::
 *   validateThrow) and follows that parabola to where it comes down (calculateParabolaVoxel);
 *   a target it can't reach shows nothing (the game will say so);
 * - a straight shot: the line of fire from the game's origin to the point under the crosshair,
 *   up to whatever it meets first (calculateLineVoxel).
 * Worked out again only when what it depends on changes. A marked target is forgotten when the
 * soldier, their place, the weapon or the mode change, or the player's turn ends.
 * @param unit The soldier being looked through (may be null).
 * @param playerTurn It's the player's turn.
 */
void FirstPersonMode::updateTrajectory(BattleUnit *unit, bool playerTurn)
{
	// The weapon and the mode the next click uses (as the crosshair works them out).
	BattleItem *weapon = (playerTurn && unit) ? activeWeapon(unit) : nullptr;
	if (weapon)
	{
		const BattleAction *current = _state->_battleGame->getCurrentAction();
		if (current->targeting && current->actor == unit && current->weapon) weapon = current->weapon;
	}
	const BattleActionType mode = weapon ? firedAs(weapon, _fireMode) : BA_NONE;

	// A marked target lasts while nothing it was marked with changes.
	if (_confirmTile.x >= 0 && (!Options::fpsConfirmFire || !playerTurn || unit != _confirmUnit || !unit || unit->getPosition() != _confirmFrom
		|| weapon != _confirmWeapon || mode != _confirmMode))
	{
		clearConfirmMark();
	}
	const bool marked = _confirmTile.x >= 0;

	// What to show: an arc for a throw or an arcing shot, a line for a straight shot (setting
	// All, or a marked target); nothing while the soldier can't act.
	const bool arcing = mode == BA_THROW || (isShot(mode) && weapon->getArcingShot(mode));
	const bool straight = isShot(mode) && !arcing && (Options::fpsTrajectory >= 2 || marked);
	const bool show = isActive() && unit && canAct() && (Options::fpsTrajectory >= 1 || marked) && (arcing || straight);
	if (!show)
	{
		if (!_trajectoryKey.empty())
		{
			_trajectoryKey.clear();
			_view->setTrajectory(std::vector<Position>());
		}
		return;
	}

	// The target: the marked one, or the tile and point under the crosshair (the tile kept
	// inside the map, as fire() takes it).
	Position targetTile = marked ? _confirmTile : _aimVoxel.toTile();
	const Position targetVoxel = marked ? _confirmVoxel : _aimVoxel;
	targetTile.x = std::clamp<int>(targetTile.x, 0, _save->getMapSizeX() - 1);
	targetTile.y = std::clamp<int>(targetTile.y, 0, _save->getMapSizeY() - 1);
	targetTile.z = std::clamp<int>(targetTile.z, 0, _save->getMapSizeZ() - 1);

	// Worked out again only when something it depends on changed (an arc depends on the
	// target's tile only, a line on the very point).
	const Position from = unit->getPosition();
	const std::vector<int> key = { arcing ? 1 : 2, targetTile.x, targetTile.y, targetTile.z, arcing ? 0 : targetVoxel.x, arcing ? 0 : targetVoxel.y,
		arcing ? 0 : targetVoxel.z, from.x, from.y, from.z, unit->getDirection(), unit->isKneeled() ? 1 : 0, weapon->getId(), (int)mode };
	if (key == _trajectoryKey)
	{
		return;
	}
	_trajectoryKey = key;

	// The game's action for it, and its path.
	TileEngine *tileEngine = _save->getTileEngine();
	BattleAction preview;
	preview.actor = unit;
	preview.weapon = weapon;
	preview.type = mode;
	preview.target = targetTile;
	std::vector<Position> path;
	if (arcing)
	{
		// The points on the tile the game tries, in its order (Projectile::calculateThrow).
		const Tile *tile = _save->getTile(targetTile);
		const Position floorPoint = targetTile.toVoxel() + Position(8, 8, 1 - tile->getTerrainLevel());
		std::vector<Position> aimPoints;
		const BattleUnit *targetUnit = mode == BA_THROW ? nullptr : tile->getOverlappingUnit(_save);
		if (mode == BA_THROW)
		{
			aimPoints.push_back(floorPoint);
		}
		else if (targetUnit && targetUnit->getVisible())
		{
			const Position unitBase = floorPoint + Position(0, 0, targetUnit->getFloatHeight());
			for (const int height : { targetUnit->getHeight() / 2 + 1, 2, targetUnit->getHeight() - 1 }) aimPoints.push_back(unitBase + Position(0, 0, height));
		}
		else if (tile->getMapData(O_OBJECT))
		{
			for (const int height : { 13, 8, 23, 2 }) aimPoints.push_back(targetTile.toVoxel() + Position(8, 8, height));
		}
		else if (tile->getMapData(O_NORTHWALL))
		{
			for (const int height : { 13, 8, 20, 3 }) aimPoints.push_back(targetTile.toVoxel() + Position(8, 0, height));
		}
		else if (tile->getMapData(O_WESTWALL))
		{
			for (const int height : { 13, 8, 20, 2 }) aimPoints.push_back(targetTile.toVoxel() + Position(0, 8, height));
		}
		else if (tile->getMapData(O_FLOOR))
		{
			aimPoints.push_back(floorPoint);
		}

		// The first point the game finds a curve for, and that curve's path.
		const Position origin = tileEngine->getOriginVoxel(preview, nullptr);
		for (const Position &aimPoint : aimPoints)
		{
			double curvature = 0.0;
			if (tileEngine->validateThrow(preview, origin, aimPoint, _save->getDepth(), &curvature))
			{
				tileEngine->calculateParabolaVoxel(origin, aimPoint, true, &path, unit, curvature, Position(0, 0, 0));
				break;
			}
		}
	}
	else
	{
		// The line of fire to the point, up to what it meets.
		const Position origin = tileEngine->getOriginVoxel(preview, _save->getTile(from));
		tileEngine->calculateLineVoxel(origin, targetVoxel, true, &path, unit);
	}
	_view->setTrajectory(path);
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
	// the menu...): this click is its target, with that item and action. Not a shot, though:
	// after a shot the game keeps aiming that same shot (so the player can fire again), and a
	// shot picked on the classic HUD has just been taken over as the fire mode (updateFireMode),
	// so a shot fires as the fire mode does, the other hand's gun joining in with dual wield.
	const bool menuTargeting = action->targeting && action->actor == unit && action->weapon && !isShot(action->type);
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

	// Doom mode's real-time grenades (setting; a first step towards a real-time mode). PRIME
	// opens the fuse box (1 to DoomFuseMaxSeconds seconds), as the isometric map's priming opens
	// its timer; the seconds picked are kept (setting "Doom grenade fuse") for every grenade after.
	// - Prime on throw (setting "Doom prime on throw", the default): that's all; the grenade is
	//   armed as it's thrown (below) and goes off that many seconds after it leaves the hand.
	// - Otherwise the pick pulls the pin, with its click, and the fuse burns from then on, in the
	//   hand too ("cooking"): throw it, or don't.
	// Without real-time grenades, grenades in Doom mode are primed and thrown as without it.
	if (mode == BA_PRIME && doomModeOn() && Options::fpsDoomRealTimeGrenades && !menuTargeting && timedGrenade(weapon))
	{
		_game->pushState(new DoomFuseState([this, unit, weapon](int seconds)
		{
			Options::fpsDoomGrenadeFuse = seconds;
			if (Options::fpsDoomPrimeOnThrow)
			{
				_state->warningRaw(_state->tr("STR_FPS_DOOM_FUSE_SET").arg(seconds));
			}
			else if (canAct())
			{
				pullDoomPin(unit, weapon, std::max<Uint32>(1, SDL_GetTicks()));
			}
		}));
		return;
	}

	// Unpriming has nothing to choose: the game's own action, at once (its time units, its
	// message and sound); a Doom fuse burning on the item stops with it.
	if (mode == BA_UNPRIME && !menuTargeting)
	{
		action->actor = unit;
		action->weapon = weapon;
		action->type = BA_UNPRIME;
		action->targeting = false;
		action->result.clear();
		action->updateTU();
		battleGame->handleNonTargetAction();
		if (weapon->getFuseTimer() == -1)
		{
			_doomFuses.erase(weapon->getId());
		}
		return;
	}

	// A medi-kit or a scanner: used at once, no menu (see useItemDirectly). Anything else used
	// or primed: the item's own action menu, as clicking it on the HUD.
	const BattleType itemType = weapon->getRules()->getBattleType();
	if (mode == BA_USE && !menuTargeting && (itemType == BT_MEDIKIT || itemType == BT_SCANNER))
	{
		useItemDirectly(unit, weapon);
		return;
	}
	if ((mode == BA_USE || mode == BA_PRIME) && !menuTargeting)
	{
		_state->handleItemClick(weapon, false);
		return;
	}

	// Melee: the game finds who is in reach in the direction the soldier faces (the 3D view
	// keeps him facing where it looks) and does the attack as its own action, which needs no
	// target click. With no one standing in reach, like the isometric map, the game's own
	// terrain melee hits the wall, door or object the soldier faces, if the mod switches it on
	// (extendedTerrainMelee); the setting "Melee hits walls and the downed" switches it on for
	// the 3D view, and first lets the blow hit a body lying in front (see meleeDowned).
	if (mode == BA_HIT)
	{
		TileEngine *tileEngine = _save->getTileEngine();
		Position meleeTile;
		action->actor = unit;
		action->weapon = weapon;
		action->terrainMeleeTilePart = 0;
		if (tileEngine->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &meleeTile))
		{
			action->target = meleeTile;
		}
		else if (Options::fpsMeleeTerrain && meleeDowned(unit, weapon, action))
		{
			return;
		}
		else
		{
			const int modTerrainMelee = Mod::EXTENDED_TERRAIN_MELEE;
			if (Options::fpsMeleeTerrain && modTerrainMelee <= 0)
			{
				Mod::EXTENDED_TERRAIN_MELEE = 1; // only for this check: parts reaching above the bottom layer
			}
			const bool terrainInReach = tileEngine->validTerrainMeleeRange(action);
			Mod::EXTENDED_TERRAIN_MELEE = modTerrainMelee;
			if (!terrainInReach)
			{
				_state->warning("STR_THERE_IS_NO_ONE_THERE");
				return;
			}
		}
		action->type = BA_HIT;
		action->targeting = false;
		action->result.clear();
		action->updateTU();
		battleGame->handleNonTargetAction();

		// The weapon in hand swings only if the game went ahead: refused (not enough time
		// units), the action carries the reason in its result.
		if (action->result.empty())
		{
			_swingStart = SDL_GetTicks();
			_swingJolted = false;
			if (Options::fpsHitSounds) _sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f); // (setting "Hit, kill and swing sounds")
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

	// Confirm fire (setting): the first click marks the target and shows its path (see
	// updateTrajectory); a click on the same tile with the same weapon and mode fires, a click on
	// another moves the mark. The held trigger's next bursts after a confirmed one go on firing.
	if (Options::fpsConfirmFire && (isShot(mode) || mode == BA_THROW) && !(_triggerHeld && _burstConfirmed))
	{
		const bool confirmed = _confirmTile == targetTile && _confirmUnit == unit && _confirmWeapon == weapon && _confirmMode == mode;
		if (!confirmed)
		{
			_confirmTile = targetTile;
			_confirmVoxel = aimedVoxel;
			_confirmFrom = unit->getPosition();
			_confirmUnit = unit;
			_confirmWeapon = weapon;
			_confirmMode = mode;
			_triggerHeld = false; // (holding the button doesn't confirm)
			_state->warning("STR_FPS_CONFIRM_FIRE_HINT");
			return;
		}
		clearConfirmMark();
		_burstConfirmed = true;
	}

	// Doom mode's real-time grenades with prime on throw: a grenade on a timer is armed as it's
	// thrown (its pin pulled, with the click), and burns for the fuse setting from the moment it
	// leaves the hand (see updateDoomFuses). One already burning (pulled before the setting
	// changed) burns on.
	if (mode == BA_THROW && doomModeOn() && Options::fpsDoomRealTimeGrenades && Options::fpsDoomPrimeOnThrow && timedGrenade(weapon)
		&& !_doomFuses.count(weapon->getId()))
	{
		pullDoomPin(unit, weapon, 0);
	}

	// Doom free grenades (setting): the grenade flies to the point under the crosshair by the 3D
	// view's own throw, wherever that is, bursting in the air if its fuse runs out first (see
	// throwFreeGrenade); the game's throw isn't asked.
	if (mode == BA_THROW && doomModeOn() && Options::fpsDoomFreeGrenades && timedGrenade(weapon))
	{
		throwFreeGrenade(unit, weapon, aimedVoxel);
		return;
	}

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
	// Leaning (setting "Lean shots", or leaning out with the lean keys): a soldier whose eye is
	// to one side of the tile's middle shoots from that side (see leanSide).
	action->relativeOrigin = (Options::fpsLeanShots || leaningOut()) && isShot(mode) ? leanSide(unit, targetTile) : BattleActionOrigin::CENTRE;

	// Dual wield (an option): the gun in the other hand fires at the same moment, round for
	// round, both half as accurate (the game halves both shots' accuracy). With time units for
	// only one of them, just the chosen hand fires, at full accuracy. (Not for an action picked
	// from a menu.)
	BattleActionType offMode = BA_NONE;
	BattleItem *otherWeapon = menuTargeting ? nullptr : dualPartner(unit, weapon, slot, offMode);
	if (otherWeapon && !doomModeOn() && action->Time + BattleActionCost(offMode, unit, otherWeapon).Time > unit->getTimeUnits())
	{
		otherWeapon = nullptr;
	}
	action->dualWield = otherWeapon != nullptr;
	action->dualWeapon = otherWeapon;
	action->dualType = otherWeapon ? offMode : BA_NONE; // (dualPartner may name a mode for a gun it turned down)

	// Free turning (no "Turning costs TU"): the soldier first faces the target for free, as
	// looking round does, so the game's own turn before the shot costs nothing. (The crosshair
	// near the edge of an eighth can aim at a tile in the next one.)
	if (!Options::fpsTurnCost && (isShot(mode) || mode == BA_THROW) && unit->directionTo(targetTile) != unit->getDirection())
	{
		unit->setDirection(unit->directionTo(targetTile));
		_save->getTileEngine()->calculateFOV(unit);
		_save->getTileEngine()->calculateFOV(unit->getPosition(), 1, false); // others' view of this unit, like UnitTurnBState
	}

	// Fire, as a click on the target tile of the isometric map: the game turns the soldier (by
	// the rules), checks and spends the time units, and launches the shot. With the game's
	// "confirm fire mode" setting the first click only marks the target, so click a second time.
	battleGame->primaryAction(targetTile);
	if (Options::battleConfirmFireMode && action->targeting && !action->waypoints.empty())
	{
		battleGame->primaryAction(targetTile);
	}

	// Clear what we added, so the next action (one from the classic HUD, say) aims normally.
	action->targetVoxel = Position(-1, -1, -1);
	action->freeAim = false;
	action->relativeOrigin = BattleActionOrigin::CENTRE;
	action->dualWield = false;
	action->dualWeapon = nullptr;
	action->dualType = BA_NONE;
}

/**
 * The patient a medi-kit would treat now. The game's own choices, as its action menu makes
 * them (and with the same rules for whom the kit may treat, standing or lying, friend, neutral
 * or foe, immune to wounds or not): a body lying on the soldier's own tile, the unit in the tile
 * the soldier faces (in reach, as for melee), or the soldier themself. The game takes the first
 * of these there is; here the player's aim picks among them:
 * - the unit in reach, when its picture is under the crosshair (or the free cursor): it (none,
 *   if the kit can't treat it);
 * - looking down (more than 25 degrees): the body at the feet;
 * - aiming at nothing in reach: the soldier themself;
 * - and when the kit can't treat the soldier, the game's own order for the rest.
 * @param unit The soldier with the kit.
 * @param medikit The kit.
 * @return The patient, or null if there's none.
 */
BattleUnit *FirstPersonMode::medikitPatient(BattleUnit *unit, BattleItem *medikit) const
{
	const RuleItem *rules = medikit->getRules();
	// Whether the kit may treat a unit of this side, standing or lying.
	auto sideAllowed = [rules](const BattleUnit *patient, bool lying) {
		const UnitFaction faction = patient->getOriginalFaction();
		return lying ? ((rules->getAllowTargetFriendGround() && faction == FACTION_PLAYER) || (rules->getAllowTargetNeutralGround() && faction == FACTION_NEUTRAL)
				|| (rules->getAllowTargetHostileGround() && faction == FACTION_HOSTILE))
			: ((rules->getAllowTargetFriendStanding() && faction == FACTION_PLAYER) || (rules->getAllowTargetNeutralStanding() && faction == FACTION_NEUTRAL)
				|| (rules->getAllowTargetHostileStanding() && faction == FACTION_HOSTILE));
	};
	// A body on the soldier's own tile (never a big unit's), unconscious and treatable.
	BattleUnit *body = nullptr;
	for (BattleUnit *other : *_save->getUnits())
	{
		if (other->getPosition() == unit->getPosition() && other != unit && other->getStatus() == STATUS_UNCONSCIOUS && !other->isBigUnit()
			&& (other->isWoundable() || rules->getAllowTargetImmune()) && rules->getAllowTargetGround() && sideAllowed(other, true))
		{
			body = other;
			break;
		}
	}
	// The unit in the tile the soldier faces, and whether the kit may treat it.
	BattleUnit *facedUnit = nullptr, *standing = nullptr;
	Position facedTile;
	if (_save->getTileEngine()->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &facedTile, false))
	{
		const Tile *tile = _save->getTile(facedTile);
		facedUnit = tile ? tile->getUnit() : nullptr;
		if (facedUnit && rules->getAllowTargetStanding() && (facedUnit->isWoundable() || rules->getAllowTargetImmune()) && sideAllowed(facedUnit, false))
		{
			standing = facedUnit;
		}
	}
	// The aim picks.
	const BattleUnit *pointed = _view->getUnitAtPointer();
	if (pointed && pointed == facedUnit)
	{
		return standing;
	}
	if (_view->getPitchDegrees() < -25.0 && body)
	{
		return body;
	}
	if (rules->getAllowTargetSelf())
	{
		return unit;
	}
	return body ? body : standing;
}

/**
 * Uses a medi-kit or a motion scanner straight from the view, with no action menu (it had only
 * the one entry). The game's own checks come first, as its menu makes them (an alien artifact
 * not researched yet, whatever stops the item being used). A scanner spends its time units and
 * shows its screen. A medi-kit treats the patient the player points at (see medikitPatient): a
 * kit of one kind (heal, stimulant or painkiller only) works at once, as the game's menu does it
 * (healing the first fatal wound, or the torso); a full kit opens the game's medi-kit screen on
 * the patient, to pick the body part and the treatment.
 * @param unit The soldier.
 * @param item The medi-kit or scanner.
 */
void FirstPersonMode::useItemDirectly(BattleUnit *unit, BattleItem *item)
{
	BattlescapeGame *battleGame = _state->_battleGame;
	battleGame->cancelCurrentAction();
	BattleAction *action = battleGame->getCurrentAction();
	action->actor = unit;
	action->weapon = item;
	action->type = BA_USE;
	action->skillRules = nullptr;
	action->targeting = false;
	action->result.clear();
	action->updateTU();
	const RuleItem *rules = item->getRules();

	// The game's checks.
	std::string result = "STR_UNKNOWN";
	if (_save->isPreview() || (unit->getOriginalFaction() == FACTION_PLAYER && !_game->getSavedGame()->isResearched(rules->getRequirements())))
	{
		_state->warning("STR_UNABLE_TO_USE_ALIEN_ARTIFACT_UNTIL_RESEARCHED");
		return;
	}
	if (!_save->canUseWeapon(item, unit, false, BA_USE, &result))
	{
		_state->warning(result);
		return;
	}

	// A scanner.
	if (rules->getBattleType() == BT_SCANNER)
	{
		if (action->spendTU(&action->result))
		{
			_game->pushState(new ScannerState(action));
		}
		else
		{
			_state->warning(action->result);
		}
		return;
	}

	// A medi-kit, on the patient pointed at.
	BattleUnit *patient = medikitPatient(unit, item);
	if (!patient)
	{
		_state->warning("STR_THERE_IS_NO_ONE_THERE");
		return;
	}
	TileEngine *tileEngine = _save->getTileEngine();
	const BattleMediKitType type = rules->getMediKitType();
	if (!type)
	{
		_game->pushState(new MedikitState(patient, action, tileEngine));
		return;
	}
	if (!((type == BMT_HEAL && item->getHealQuantity() > 0) || (type == BMT_STIMULANT && item->getStimulantQuantity() > 0)
		|| (type == BMT_PAINKILLER && item->getPainKillerQuantity() > 0)))
	{
		_state->warning("STR_NO_USES_LEFT");
		return;
	}
	if (!action->spendTU(&action->result))
	{
		_state->warning(action->result);
		return;
	}
	UnitBodyPart part = BODYPART_TORSO;
	if (type == BMT_HEAL)
	{
		for (int bodyPart = 0; bodyPart < BODYPART_MAX; ++bodyPart)
		{
			if (patient->getFatalWound((UnitBodyPart)bodyPart))
			{
				part = (UnitBodyPart)bodyPart;
				break;
			}
		}
	}
	tileEngine->medikitUse(action, patient, type == BMT_HEAL ? BMA_HEAL : type == BMT_STIMULANT ? BMA_STIMULANT : BMA_PAINKILLER, part);
	tileEngine->medikitRemoveIfEmpty(action);
}

/**
 * Leaning (setting "Lean shots", or leaning out with the lean keys): which side of the soldier a
 * shot starts from. In the 3D view the soldier may stand off the middle of their tile, and lean
 * out further (see updateLean); with the eye more than LeanThreshold voxels to one side of the
 * middle, across the line to the target, the shot starts from that side: the game's own
 * off-centre origins (BattleActionOrigin LEFT / RIGHT, see TileEngine::getOriginVoxel, the ones
 * its "off-centre shooting" option tries when the middle has no line of fire), so the offset
 * stays within the game's limit. Peeking round a corner.
 * @param unit The soldier.
 * @param targetTile The tile aimed at.
 * @return The side the shot starts from: the middle, left or right.
 */
BattleActionOrigin FirstPersonMode::leanSide(const BattleUnit *unit, const Position &targetTile) const
{
	// The way to the target on the map (x east, y south), and where the eye is (with any lean)
	// from the middle of the soldier's tile (a big unit's middle is half a tile further on).
	const int sizeOffset = 8 * (unit->getArmor()->getSize() - 1);
	const double towardsX = targetTile.x - unit->getPosition().x, towardsY = targetTile.y - unit->getPosition().y;
	const double length = std::sqrt(towardsX * towardsX + towardsY * towardsY);
	if (length < 0.5)
	{
		return BattleActionOrigin::CENTRE;
	}
	const double offsetX = _eyeX + _leanShift[0] - (unit->getPosition().x * 16.0 + 8.0 + sizeOffset),
		offsetY = _eyeY + _leanShift[1] - (unit->getPosition().y * 16.0 + 8.0 + sizeOffset);
	// How far to the left of the line to the target the eye is (left of a heading
	// (x, y) is (y, -x) on a map with y pointing south), in voxels.
	const double toTheLeft = (offsetX * towardsY - offsetY * towardsX) / length;
	if (toTheLeft > LeanThreshold) return BattleActionOrigin::LEFT;
	if (toTheLeft < -LeanThreshold) return BattleActionOrigin::RIGHT;
	return BattleActionOrigin::CENTRE;
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
	const double focalPixels = (viewWidth / 2.0) / std::tan(fieldOfViewNow() * Pi / 360.0);
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
	case BA_UNPRIME:
		// An item used from its menu (or switched off): a box.
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
 * accuracy, then its range dropoff, a mod's penalty for a target out of the soldier's line of
 * sight, and its maximum range), so the two agree. The
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
		bool canFire = doomModeOn() || cost.Time <= unit->getTimeUnits();
		if (isShot(mode) || mode == BA_LAUNCH)
		{
			canFire = canFire && (!weapon->needsAmmoForAction(mode) || weapon->getAmmoForAction(mode));
		}

		// Dual wield: the other hand's gun joins in if it can fire along (dualPartner: a fitting
		// mode, loaded) and there's time for both; otherwise the chosen gun fires alone, at full
		// accuracy, as fire() decides too.
		BattleActionType offMode = BA_NONE;
		int offTime = 0;
		if (BattleItem *otherWeapon = canFire ? dualPartner(unit, weapon, slot, offMode) : nullptr)
		{
			offTime = BattleActionCost(offMode, unit, otherWeapon).Time;
			dual = doomModeOn() || cost.Time + offTime <= unit->getTimeUnits();
		}

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
			// A mod may cut the accuracy of a shot at what the soldier can't see to a share of it
			// (noLOSAccuracyPenalty, in percent; -1 for none), as the shot and the cursor do.
			const int noSightPenalty = rules->getNoLOSAccuracyPenalty(_game->getMod());
			if (noSightPenalty != -1 && !aimedTileInSight(unit, weapon, mode, _aimVoxel.toTile()))
			{
				accuracy = accuracy * noSightPenalty / 100;
				accuracyColor = yellowText;
			}
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
			{ BA_PRIME, "STR_FPS_MODE_PRIME" }, { BA_UNPRIME, "STR_FPS_MODE_UNPRIME" } };
		for (const auto &entry : modeNames)
		{
			if (entry.mode == mode) modeText = _state->tr(entry.name);
		}
		// Doom real-time grenades primed on throw: PRIME sets the fuse for every grenade, "FUSE 3s".
		if (mode == BA_PRIME && doomModeOn() && Options::fpsDoomRealTimeGrenades && Options::fpsDoomPrimeOnThrow && timedGrenade(weapon))
		{
			modeText = _state->tr("STR_FPS_DOOM_FUSE").arg(std::clamp(Options::fpsDoomGrenadeFuse, 1, FirstPerson::DoomFuseMaxSeconds));
		}
		// A medi-kit names its patient: "USE: Fred", "USE: SELF" (see medikitPatient); with
		// none, it can't be used.
		if (mode == BA_USE && rules->getBattleType() == BT_MEDIKIT)
		{
			const BattleUnit *patient = medikitPatient(unit, weapon);
			modeText += ": " + (patient == unit ? std::string(_state->tr("STR_FPS_MEDIKIT_SELF")) : patient ? unitLabel(patient) : std::string("-"));
			canFire = canFire && patient;
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

	// A hit (or a kill) on an enemy in sight ticks once (setting "Hit and kill sounds") and marks
	// the crosshair for 280 ms.
	const Uint32 hitTick = _view->getHitMarkTick();
	if (hitTick != _hitMarkerHeard)
	{
		_hitMarkerHeard = hitTick;
		if (Options::fpsHitSounds)
		{
			_sounds->play(_view->getHitMarkKill() ? FirstPersonSounds::KILL : FirstPersonSounds::HIT, 0, 0.7f);
		}
	}
	const int hitMark = hitTick && SDL_GetTicks() - hitTick < 280 ? (_view->getHitMarkKill() ? 2 : 1) : 0;
	const Uint8 hitColor = hitMark == 2 ? red : nearestColor(palette, 255, 255, 255);

	// FPS style: a dotted ring shows the spread (it grows with the bloom of the last rounds).
	// Its radius in pixels is the spread angle seen through the view's lens, kept inside the
	// crosshair surface.
	int ringRadius = 0;
	if (unit && freeAimOn(mode))
	{
		const double focalPixels = (_view->getWidth() / 2.0) / std::tan(fieldOfViewNow() * Pi / 360.0);
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
 * Tells whether the soldier has a line of sight to the aimed tile, checked as a shot checks it
 * for a mod's penalty on shots at what the soldier can't see (Projectile::applyAccuracy): for a
 * tile with a creature on it, whether the soldier sees that creature; for any other tile,
 * whether a line from the soldier reaches it. The answer is kept while the crosshair stays on
 * the same tile and the soldier doesn't move, turn or change the fire mode, for at most a
 * quarter of a second (creatures move).
 * @param unit The soldier.
 * @param weapon The weapon he aims.
 * @param mode The fire mode (it decides where the line starts).
 * @param tile The tile under the crosshair.
 * @return True if he can see it (or it's off the map, where the shot takes no penalty either).
 */
bool FirstPersonMode::aimedTileInSight(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const Position &tile)
{
	Tile *aimedTile = _save->getTile(tile);
	if (!aimedTile)
	{
		return true;
	}

	// The last answer, if it was for the same question just now.
	const Uint32 now = SDL_GetTicks();
	if (unit == _sightCheckUnit && unit->getPosition() == _sightCheckFrom && tile == _sightCheckTile
		&& unit->getDirection() == _sightCheckDirection && mode == _sightCheckMode && now - _sightCheckTime < 250)
	{
		return _sightCheckResult;
	}

	// Ask the game, as the shot does.
	if (aimedTile->getUnit())
	{
		_sightCheckResult = _save->getTileEngine()->visible(unit, aimedTile);
	}
	else
	{
		BattleAction sightAction;
		sightAction.actor = unit;
		sightAction.weapon = weapon;
		sightAction.type = mode;
		_sightCheckResult = _save->getTileEngine()->isTileInLOS(&sightAction, aimedTile, false);
	}

	// Remember what it was for.
	_sightCheckUnit = unit;
	_sightCheckFrom = unit->getPosition();
	_sightCheckTile = tile;
	_sightCheckDirection = unit->getDirection();
	_sightCheckMode = mode;
	_sightCheckTime = now;
	return _sightCheckResult;
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
 * Where the player points on the view: the crosshair, in the middle, with mouse look; the free
 * cursor otherwise (the view lies on our canvas, so its pixels are the canvas's).
 * @param pointerX, pointerY Set to the pixel on the view.
 */
void FirstPersonMode::pointer(int &pointerX, int &pointerY) const
{
	pointerX = _mouseLook ? _view->getWidth() / 2 : _cursorX;
	pointerY = _mouseLook ? _view->getHeight() / 2 : _cursorY;
}

/**
 * What the game's own unit screen (the middle click on the map) calls a unit: a civilian by
 * their kind, an alien by their race (or race and rank, if the mod shows full names there), a
 * soldier by their name.
 * @param target The unit.
 * @return The name.
 */
std::string FirstPersonMode::unitLabel(const BattleUnit *target) const
{
	if (target->getOriginalFaction() == FACTION_NEUTRAL)
	{
		return _state->tr(target->getType());
	}
	if (target->getUnitRules())
	{
		return target->getUnitRules()->getShowFullNameInAlienInventory(_game->getMod()) ? target->getName(_game->getLanguage())
			: std::string(_state->tr(target->getUnitRules()->getRace()));
	}
	return target->getName(_game->getLanguage());
}

/**
 * Every frame in 3D: names the unit the player points at (setting "Unit names": the one whose
 * picture is under the crosshair or the free cursor, see FirstPersonView::getUnitAtPointer) just
 * under the crosshair, or the cursor, in its side's colour (green ours, red the aliens', yellow the
 * civilians'), as the game's unit screen names it; and tells the view where the player points,
 * and how much text is under it there (the icons over a downed unit go below it).
 * @param unit The soldier looked through, or null (nothing named).
 */
void FirstPersonMode::updateUnitName(BattleUnit *unit)
{
	int pointerX, pointerY;
	pointer(pointerX, pointerY);
	const BattleUnit *target = Options::fpsUnitNames && isActive() && unit ? _view->getUnitAtPointer() : nullptr;
	_txtTarget->setVisible(target != nullptr);
	_view->setPointer(pointerX, pointerY, target ? _txtTarget->getHeight() + 1 : 0);
	if (!target)
	{
		return;
	}
	const SDL_Color *palette = _view->getPalette();
	const Uint8 color = target->getFaction() == FACTION_PLAYER ? textColor(palette, 130, 230, 100)
		: target->getFaction() == FACTION_HOSTILE ? textColor(palette, 240, 70, 50) : textColor(palette, 255, 216, 64);
	if (_txtTarget->getColor() != color) _txtTarget->setColor(color);
	const std::string label = unitLabel(target);
	if (_txtTarget->getText() != label) _txtTarget->setText(label);
	// Centred under the crosshair, clear of its picture (about 8 pixels round the middle).
	_txtTarget->setX(_view->getX() + pointerX - _txtTarget->getWidth() / 2);
	_txtTarget->setY(_view->getY() + pointerY + 10);
}

/**
 * The middle click as on the isometric map (setting "Middle click identifies"): on a unit the
 * player points at (one in sight, standing, its picture under the crosshair or the cursor: see
 * FirstPersonView::getUnitAtPointer), the game's own unit screen (who it is and what it
 * carries); in debug mode with Ctrl held, the mind probe's screen, as the map does.
 * @return True if a unit was shown; false if there's none there (the click frees or takes
 *         back the cursor as before).
 */
bool FirstPersonMode::identifyPointedUnit()
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!Options::fpsMiddleClickIdentify || !unit || _state->_battleGame->isBusy())
	{
		return false;
	}
	BattleUnit *target = _view->getUnitAtPointer();
	if (!target)
	{
		return false;
	}
	_state->_battleGame->cancelCurrentAction();
	if (_save->getDebugMode() && _game->isCtrlPressed())
	{
		_state->popup(new UnitInfoState(target, _state, false, true));
	}
	else
	{
		_game->pushState(new AlienInventoryState(target));
	}
	return true;
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
		if (weapon && weapon->getAmmoForAction(BA_AUTOSHOT) && (doomModeOn() || cost.haveTU()))
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
