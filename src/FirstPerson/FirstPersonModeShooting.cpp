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
#include "../Battlescape/PrimeGrenadeState.h"
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
 * Holds the game's timer box (PrimeGrenadeState) when the 3D view primes a grenade, in the place
 * of the item's action menu the box expects under it: a timer picked closes the box and the menu
 * together (on the map, back to the battle), so here it closes the box and this; a cancelled box
 * closes only itself, and this then closes too, as soon as it is on top again. It shows nothing
 * of its own (the battle shows through).
 */
class PrimeTimerHost : public State
{
public:
	/**
	 * Makes the host for priming the current action's grenade.
	 * @param action The game's current action, set up to prime (see FirstPersonMode::fire).
	 */
	explicit PrimeTimerHost(BattleAction *action) : _action(action)
	{
		_screen = false;
	}
	/**
	 * On top: the first time, opens the timer box over itself; after that (the box cancelled),
	 * closes.
	 */
	void init() override
	{
		State::init();
		if (!_boxShown)
		{
			_boxShown = true;
			_game->pushState(new PrimeGrenadeState(_action, false, nullptr));
		}
		else
		{
			_game->popState();
		}
	}
private:
	BattleAction *_action;  // the game's current action (priming)
	bool _boxShown = false; // the timer box has been opened
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

	// The hit (its power and damage type as the game works them out), its sound and the swing,
	// the casualties, and any reaction fire the time spent draws.
	const BattleActionAttack attack = BattleActionAttack::GetAferShoot(*action, ammo);
	const RuleDamageType *damageType = nullptr;
	const int power = meleeBlowPower(attack, damageType);
	meleeBlowLanded(weapon);
	_save->getTileEngine()->hitUnit(attack, body, Position(0, 0, 0), damageType->getRandomDamage(power), damageType, false);
	_state->_battleGame->checkForCasualties(damageType, attack, false, false);
	_save->getTileEngine()->checkReactionFire(unit, *action);
	action->type = BA_NONE;
	return true;
}

/**
 * Melee at the floor or ceiling under the crosshair (a player's report, GitHub issue #33: melee
 * "doesn't apply to floors sections above or bellow"; the owner: with terrain melee on, as the
 * setting "Melee hits floors" says, see crosshairTerrainMelee). The game's terrain melee knows no
 * floors (a floor is the tile part it reads as none), so the blow is the game's own pieces put
 * together, as for a body lying down (see meleeDowned): the soldier turns to the floor's tile
 * first (see turnBeforeUse; none for their own), the time units and ammo the game's melee
 * spends, the weapon's melee power and damage type, its melee sound, the game's hit at the point
 * struck (TileEngine::hit: the part found from the point, the floor; a body lying on it takes the
 * blow first, as with a shot at the floor; the floor's armour, and if it breaks the game's
 * falling and light; terrain melee doesn't miss), the game's check for casualties, and reaction
 * fire.
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @param action The game's current action, to fill in.
 * @param struck The point of the floor the crosshair meets.
 */
void FirstPersonMode::meleeFloor(BattleUnit *unit, BattleItem *weapon, BattleAction *action, const Position &struck)
{
	// The cost: the game's time units for the blow (and the turn before it, both wanted before
	// either is spent) and the weapon's ammo for it.
	action->type = BA_HIT;
	action->targeting = false;
	action->target = struck.toTile();
	action->result.clear();
	action->updateTU();
	BattleItem *ammo = weapon->getAmmoForAction(BA_HIT, &action->result);
	if (!ammo || !action->haveTU(&action->result))
	{
		_state->warning(action->result.empty() ? "STR_NOT_ENOUGH_TIME_UNITS" : action->result);
		action->type = BA_NONE;
		return;
	}
	if (!turnBeforeUse(unit, unit->directionTo(action->target), action->Time))
	{
		action->type = BA_NONE;
		return;
	}
	action->spendTU(&action->result);
	weapon->spendAmmoForAction(BA_HIT, _save);

	// The hit, its sound and the swing, the casualties, and any reaction fire the time spent draws.
	const BattleActionAttack attack = BattleActionAttack::GetAferShoot(*action, ammo);
	const RuleDamageType *damageType = nullptr;
	const int power = meleeBlowPower(attack, damageType);
	meleeBlowLanded(weapon);
	_save->getTileEngine()->hit(attack, struck, power, damageType, false, 0);
	_state->_battleGame->checkForCasualties(damageType, attack, false, false);
	_save->getTileEngine()->checkReactionFire(unit, *action);
	action->type = BA_NONE;
}

/**
 * A melee blow's power and damage type, as the game works them out for a melee hit: a melee
 * weapon's own power and damage, or a gun's melee bonus and melee damage (a rifle butt).
 * @param attack The blow.
 * @param damageType Set to its damage type.
 * @return Its power.
 */
int FirstPersonMode::meleeBlowPower(const BattleActionAttack &attack, const RuleDamageType *&damageType) const
{
	const RuleItem *damageRule = attack.damage_item->getRules();
	const bool meleeWeapon = damageRule->getBattleType() == BT_MELEE;
	damageType = meleeWeapon ? damageRule->getDamageType() : damageRule->getMeleeType();
	return meleeWeapon ? damageRule->getPowerBonus(attack) : damageRule->getMeleeBonus(attack);
}

/**
 * A blow the 3D view makes of the game's own pieces lands (see meleeDowned, meleeFloor): the
 * weapon's melee hit sound (or its swing's), and the weapon in hand swings, as for any blow that
 * lands, with our swoosh (setting "Hit, kill and swing sounds").
 * @param weapon The weapon striking.
 */
void FirstPersonMode::meleeBlowLanded(BattleItem *weapon)
{
	int sound = weapon->getRules()->getMeleeHitSound();
	if (sound < 0) sound = weapon->getRules()->getMeleeSound();
	if (sound >= 0) _state->_battleGame->playSound(sound);
	_swingStart = SDL_GetTicks();
	_swingJolted = false;
	if (Options::fpsHitSounds) _sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f);
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
	// The options file keeps it, so the options screen's Cancel doesn't quietly undo it.
	rememberChoices();
	Options::save();
}

/**
 * The akimbo style key: switches two guns between taking turns round by round (each round
 * resolved before the next leaves, as an auto burst's) and firing as a volley (both rounds at
 * once, landing together; setting "Akimbo volley"). The game's warning line says which.
 */
void FirstPersonMode::toggleAkimboStyle()
{
	Options::fpsAkimboVolley = !Options::fpsAkimboVolley;
	_state->warning(Options::fpsAkimboVolley ? "STR_FPS_AKIMBO_TOGETHER_ON" : "STR_FPS_AKIMBO_IN_TURN_ON");
}

/**
 * Akimbo on the map (the owner: "if we have weapon in each arm there should be a key combo ctrl
 * shift plus click"): do the soldier's two guns pair up there? Both hands must hold plain guns (not
 * a guided launcher, steered by its waypoints), two of them; with "Realistic akimbo" neither may
 * be two-handed. "Full akimbo" needn't be on: the mark on the map is the choice.
 * @param unit The soldier.
 * @return True if they pair up.
 */
bool FirstPersonMode::mapAkimboPairs(const BattleUnit *unit) const
{
	const BattleItem *right = unit ? unit->getRightHandWeapon() : nullptr, *left = unit ? unit->getLeftHandWeapon() : nullptr;
	auto plainGun = [](const BattleItem *item) { return item && item->getRules()->getBattleType() == BT_FIREARM && item->getCurrentWaypoints() == 0; };
	if (!plainGun(right) || !plainGun(left) || right == left)
	{
		return false;
	}
	return !(Options::fpsAkimboRealistic && (right->getRules()->isTwoHanded() || left->getRules()->isTwoHanded()));
}

/**
 * Akimbo on the map: the other hand's gun that fires along with the chosen one's mode there, as in
 * 3D (see dualPartner): a shot's (no aimed one: that fires alone, as in 3D), the other gun's same
 * mode if it has it, else its first shot, and only if it's loaded for it; with the game's own
 * targeting, as the map's shots have. Neither may arc (two arcs fire in turn
 * in 3D, by the view's own doing).
 * @param unit The soldier.
 * @param weapon The gun fired.
 * @param slot Its fire mode.
 * @param partnerMode Set to the mode the other gun fires.
 * @return The other gun, or null if it doesn't fire along.
 */
BattleItem *FirstPersonMode::mapAkimboPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &partnerMode) const
{
	if (!isShot(slot) || slot == BA_AIMEDSHOT || !mapAkimboPairs(unit) || (weapon != unit->getRightHandWeapon() && weapon != unit->getLeftHandWeapon())
		|| weapon->getArcingShot(slot))
	{
		return nullptr;
	}
	BattleItem *otherWeapon = weapon == unit->getRightHandWeapon() ? unit->getLeftHandWeapon() : unit->getRightHandWeapon();
	const std::vector<BattleActionType> otherModes = fireModesFor(unit, otherWeapon);
	partnerMode = BA_NONE;
	if (std::find(otherModes.begin(), otherModes.end(), slot) != otherModes.end())
	{
		partnerMode = slot;
	}
	else
	{
		for (BattleActionType candidate : otherModes)
		{
			if (isShot(candidate)) { partnerMode = candidate; break; }
		}
	}
	if (partnerMode == BA_NONE || otherWeapon->getArcingShot(partnerMode) || !otherWeapon->getAmmoForAction(partnerMode))
	{
		return nullptr;
	}
	return otherWeapon;
}

/**
 * Akimbo on the map (the owner: Ctrl and Shift held, a click on a hand's weapon; "a mini target
 * should appear next to each weapon"): steps the soldier's two guns through firing together on
 * the map, both at once (one mini target by each), in turn (two), and not, the game's warning line
 * saying which; kept with the soldier in the save. Without two guns that pair up, the line says
 * so. Shots in 3D keep their own akimbo (the settings, the style key).
 * @param state The battlescape screen.
 * @param unit The selected soldier.
 * @return True if the click was taken (the add-on is on).
 */
bool FirstPersonMode::toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || !unit)
	{
		return false;
	}
	if (!mode->mapAkimboPairs(unit))
	{
		unit->setMapAkimbo(0);
		state->warning("STR_FPS_MAP_AKIMBO_TWO_GUNS");
		return true;
	}
	const int next = (unit->getMapAkimbo() + 1) % 3;
	unit->setMapAkimbo(next);
	state->warning(next == 1 ? "STR_FPS_AKIMBO_TOGETHER_ON" : next == 2 ? "STR_FPS_AKIMBO_IN_TURN_ON" : "STR_FPS_AKIMBO_OFF");
	return true;
}

/**
 * Akimbo on the map: its mini targets in both hands' boxes on the classic HUD (one each for both
 * guns at once, two for in turn), drawn after the game draws the hands. Once the two no longer
 * pair up (a gun has left a hand) it's undone.
 * @param state The battlescape screen.
 * @param unit The selected soldier (null: none the player can use).
 * @param leftHand The left hand's box.
 * @param rightHand The right hand's box.
 */
void FirstPersonMode::drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || !unit || !unit->getMapAkimbo())
	{
		return;
	}
	if (!mode->mapAkimboPairs(unit))
	{
		unit->setMapAkimbo(0);
		return;
	}
	for (Surface *hand : { leftHand, rightHand })
	{
		const Uint8 color = nearestColor(hand->getPalette(), MapAkimboMarkRed, MapAkimboMarkGreen, MapAkimboMarkBlue);
		hand->lock();
		for (int mark = 0; mark < unit->getMapAkimbo(); ++mark)
		{
			for (int row = 0; row < 7; ++row)
				for (int column = 0; column < 7; ++column)
				{
					if (MapAkimboMark[row][column] == 'X') hand->setPixel(MapAkimboMarkX + mark * MapAkimboMarkStep + column, MapAkimboMarkY + row, color);
				}
		}
		hand->unlock();
	}
}

/**
 * A shot fired on the map (BattlescapeGame::primaryAction, as it launches it): with akimbo marked
 * there for the soldier (see toggleMapAkimbo), the other hand's gun fires along, as in 3D (see
 * fire): both at once or in turn, both less accurate, for the pair's cost (the other gun fires
 * only if there are time units for its share). Anything akimbo left on the game's action is
 * cleared first. Not while the 3D view shows: its shots go through here too, with their own.
 * @param state The battlescape screen.
 * @param action The shot.
 */
void FirstPersonMode::mapAkimbo(BattlescapeState *state, BattleAction &action)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || mode->isActive())
	{
		return;
	}
	action.dualWield = false;
	action.dualVolley = false;
	action.dualWeapon = nullptr;
	action.dualType = BA_NONE;
	BattleUnit *unit = action.actor;
	if (!unit || !unit->getMapAkimbo() || !action.weapon)
	{
		return;
	}
	BattleActionType partnerMode = BA_NONE;
	if (BattleItem *partner = mode->mapAkimboPartner(unit, action.weapon, action.type, partnerMode))
	{
		action.dualWield = true;
		action.dualWeapon = partner;
		action.dualType = partnerMode;
		action.dualVolley = unit->getMapAkimbo() == 1;
	}
}

/**
 * Dual wield: finds the other hand's gun if it can fire along with the chosen gun's fire mode,
 * and works out which of its own modes it fires: the same mode if it has it, else its first
 * shot mode. Each gun pays its own time units for its own mode (by the game's rules).
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @param mode The fire mode picked for the chosen weapon.
 * @param partnerMode Set to the fire mode the other gun fires.
 * @return The other hand's gun, or null if it can't join in (no gun, no shot mode that fits,
 *         or no ammo).
 */
BattleItem *FirstPersonMode::dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType mode, BattleActionType &partnerMode) const
{
	// Only shots are fired with both hands (not throws, melee, psi...), and no aimed one (one
	// picked on the classic HUD all the same fires the chosen gun alone, see fireModesFor).
	BattleItem *otherWeapon = isShot(mode) && mode != BA_AIMEDSHOT ? offHandWeapon(unit, weapon) : nullptr;
	if (!otherWeapon)
	{
		return nullptr;
	}

	// Pick the other gun's mode: the same as the chosen gun's if it has it, else its first shot.
	const std::vector<BattleActionType> otherModes = fireModesFor(unit, otherWeapon);
	BattleActionType pickedMode = BA_NONE;
	if (std::find(otherModes.begin(), otherModes.end(), mode) != otherModes.end())
	{
		pickedMode = mode;
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
	partnerMode = pickedMode;

	// It fires its own mode's full burst, however many rounds the chosen gun's has (taking turns,
	// or as a volley with the rounds it has left over following on their own; see
	// ProjectileFlyBState::takeTurn): a player's report, a 5-round gun beside a 4-round one fell
	// back to a single snap shot here.

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
			if (!isLauncher && rules->getCostAimed().Time > 0 && !offHandWeapon(unit, weapon)) modes.push_back(BA_AIMEDSHOT);
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
	// classic HUD, so take it over. Not, though, the other gun's shot fired in turn just now
	// (akimbo, see fireTurnShot): the hand stays.
	const bool ownTurnShot = action->weapon && action->weapon == _turnShotFired;
	if (action->targeting && action->actor == unit && action->weapon && action->type != BA_NONE && !ownTurnShot)
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
 * Steps to the next or previous fire mode of the weapon in hand (mouse wheel, or J by
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

	// Only a mode the weapon offers; picking it calls off anything being aimed. A mode it
	// doesn't have: a line says so, so the key doesn't seem dead.
	const std::vector<BattleActionType> modes = fireModesFor(unit, activeWeapon(unit));
	if (std::find(modes.begin(), modes.end(), mode) != modes.end())
	{
		_state->_battleGame->cancelCurrentAction();
		_fireMode = mode;
		_fireModeWeapon = activeWeapon(unit);
	}
	else if (mode == BA_AIMEDSHOT && offHandWeapon(unit, activeWeapon(unit)))
	{
		_state->warningRaw(_state->tr("STR_FPS_AKIMBO_NO_AIMED").arg(keyLabel(Options::keyFpsAkimbo))); // (see fireModesFor)
	}
	else
	{
		_state->warning("STR_FPS_NO_SUCH_MODE");
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
 * Whom a melee blow would strike now, by the crosshair (a player's report, "Melee locked to compass
 * facing": the crosshair on a cabinet to the north-west, the soldier facing north, the blow struck
 * the unit to the north, and the other way round; the owner: the crosshair first, as the medi-kit
 * aims, see medikitPatient): a unit standing under the crosshair (or the free cursor: its
 * hitbox or picture, see updatePointedUnit) on a tile
 * next to the soldier, by the game's melee reach (TileEngine::validMeleeRange, any of the eight
 * ways: the one needing the fewest eighths of a turn), the soldier first turning to face it; a
 * unit farther off, out of range. A unit straight above or below the soldier (on a lift, in a
 * shaft, flying just above; GitHub issue #33), which the game's melee never reaches: struck if the
 * game's own line from the blow's origin reaches it (validMeleeRange's origin and test,
 * TileEngine::canTargetUnit), nothing solid in between (a UFO lift's upper pad is a floor
 * between, so a unit on the next level stays out of reach), without a turn. With no unit under the
 * crosshair, none (fire goes on to the wall, door or object under it, then to the way the soldier
 * faces).
 * @param unit The soldier striking.
 * @return The unit struck (null if none, with the reason when it's out of reach), the tile it's
 *   struck on and the way to turn to it.
 */
FirstPersonMode::MeleeAim FirstPersonMode::meleeTarget(BattleUnit *unit) const
{
	MeleeAim aim;
	BattleUnit *pointed = pointedUnit();
	if (!pointed || pointed == unit)
	{
		return aim;
	}
	int bestEighths = 9;
	for (int direction = 0; direction < 8; ++direction)
	{
		Position reachedTile;
		const int eighths = std::min((direction - unit->getDirection() + 8) % 8, (unit->getDirection() - direction + 8) % 8);
		if (eighths < bestEighths && _save->getTileEngine()->validMeleeRange(unit->getPosition(), direction, unit, pointed, &reachedTile))
		{
			bestEighths = eighths;
			aim.direction = direction;
			aim.tile = reachedTile;
		}
	}
	// Straight above or below: the game's line from the blow's origin (as validMeleeRange's: the
	// tile's middle, 4 voxels under the top of the unit, and the armor's melee offset).
	const Position at = unit->getPosition();
	const int size = unit->getArmor()->getSize();
	for (int x = 0; x < size && aim.direction < 0; ++x)
	{
		for (int y = 0; y < size && aim.direction < 0; ++y)
		{
			for (const int dz : { 1, -1 })
			{
				Tile *origin = _save->getTile(at + Position(x, y, 0));
				Tile *target = _save->getTile(at + Position(x, y, dz));
				if (!origin || !target || target->getUnit() != pointed)
				{
					continue;
				}
				Position originVoxel = origin->getPosition().toVoxel() + Position(8, 8, unit->getHeight() + unit->getFloatHeight() - 4
					- origin->getTerrainLevel() + unit->getArmor()->getMeleeOriginVoxelVerticalOffset());
				Position targetVoxel;
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, target, &targetVoxel, unit, false))
				{
					aim.direction = unit->getDirection();
					aim.tile = target->getPosition();
					break;
				}
			}
		}
	}
	if (aim.direction < 0)
	{
		aim.refusal = "STR_OUT_OF_RANGE";
		return aim;
	}
	aim.target = pointed;
	return aim;
}

/**
 * Turns the soldier to face a way before using something on a neighbour (a medi-kit's patient, a
 * melee blow's target or the terrain it strikes, see useItemDirectly and fire). With "Turning
 * costs TU" an eighth at a time the shorter way round, each the armor's turn cost (as the game's
 * turn charges it; nothing in Doom mode), the turn and the use both wanting their time units
 * before either is spent; with free turning for nothing.
 * @param unit The soldier.
 * @param direction The way to face (-1, or the way faced already: no turn).
 * @param useTime The time units the use takes after the turn.
 * @return False, with the game's warning, if the time units won't cover the turn and the use.
 */
bool FirstPersonMode::turnBeforeUse(BattleUnit *unit, int direction, int useTime)
{
	if (direction < 0 || direction == unit->getDirection())
	{
		return true;
	}
	const int eighths = std::min((direction - unit->getDirection() + 8) % 8, (unit->getDirection() - direction + 8) % 8);
	const int turnCost = Options::fpsTurnCost && !doomModeOn() ? eighths * unit->getTurnCost() : 0;
	if (turnCost > 0 && unit->getTimeUnits() < turnCost + useTime)
	{
		_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		return false;
	}
	unit->spendTimeUnits(turnCost);
	unit->setDirection(direction);
	TileEngine *tileEngine = _save->getTileEngine();
	tileEngine->calculateFOV(unit);
	tileEngine->calculateFOV(unit->getPosition(), 1, false); // others' view of this unit, as after the game's turn
	_state->updateSoldierInfo(false);
	return true;
}

/**
 * Whether a melee blow may strike terrain: the mod switches the game's terrain melee on
 * (extendedTerrainMelee) or the setting "Melee hits walls and the downed" does for the 3D view,
 * and the weapon damages terrain at all (a melee weapon's own damage, else its melee's; as the
 * game's own check, validTerrainMeleeRange).
 * @param weapon The weapon (or fist) striking.
 * @return True if the blow may strike a wall, door or object.
 */
bool FirstPersonMode::meleeStrikesTerrain(BattleItem *weapon) const
{
	if (!Options::fpsMeleeTerrain && Mod::EXTENDED_TERRAIN_MELEE <= 0)
	{
		return false;
	}
	const RuleItem *rules = weapon ? weapon->getRules() : nullptr;
	return !rules || (rules->getBattleType() == BT_MELEE ? rules->getDamageType() : rules->getMeleeType())->ToTile != 0.0;
}

/**
 * The terrain a melee blow at the crosshair meets (a player's report: melee couldn't strike a
 * structure diagonally in front, "Preferably you should be able to melee any voxel structure
 * within around 1 tile of the player, and base it entirely on where first person crosshair is";
 * on stairs the walls of both floors ahead): what the crosshair meets, by the game's own line
 * (see aimAt), if it's a wall, a door or an object (the game's terrain melee knows no floors), no
 * farther from the eye than MeleeTerrainReach, on the unit's own tile or one next to it (the eight
 * around, a level up or down too: the owner's choice). The game's terrain melee then strikes that
 * part of that tile, if the blow may strike terrain (see meleeStrikesTerrain); if it may not, the
 * blow strikes nothing (a player's report: the crosshair on a cabinet, the blow struck the unit
 * the soldier faced). Floors too when asked (setting "Melee hits floors", with terrain melee on:
 * the floor at the feet or around, the ceiling above; see meleeFloor).
 * @param unit The soldier striking.
 * @param struck Set to the voxel the crosshair meets.
 * @param part Set to the part met (V_WESTWALL, V_NORTHWALL, V_OBJECT, or V_FLOOR if asked).
 * @param floors Whether a floor counts.
 * @return True if the crosshair meets a wall, door or object (or floor) within reach.
 */
bool FirstPersonMode::crosshairTerrainMelee(BattleUnit *unit, Position &struck, int &part, bool floors) const
{
	// What the crosshair meets: a wall, a door or an object (or a floor), within reach.
	int hitType = V_EMPTY;
	if (!aimAt(_view->getWidth() / 2, _view->getHeight() / 2, unit, struck, &hitType)
		|| (hitType != V_WESTWALL && hitType != V_NORTHWALL && hitType != V_OBJECT && !(floors && hitType == V_FLOOR)))
	{
		return false;
	}
	double eye[3];
	_view->getEye(eye);
	const double dx = struck.x + 0.5 - eye[0], dy = struck.y + 0.5 - eye[1], dz = struck.z + 0.5 - eye[2];
	if (dx * dx + dy * dy + dz * dz > MeleeTerrainReach * MeleeTerrainReach)
	{
		return false;
	}
	// On the unit's own tile or one next to it, a level up or down included.
	const Position tile = struck.toTile(), at = unit->getPosition();
	const int size = unit->getArmor()->getSize();
	if (tile.x < at.x - 1 || tile.x > at.x + size || tile.y < at.y - 1 || tile.y > at.y + size || std::abs(tile.z - at.z) > 1)
	{
		return false;
	}
	const Tile *target = _save->getTile(tile);
	if (!target || !target->getMapData((TilePart)hitType))
	{
		return false;
	}
	part = hitType;
	return true;
}

/**
 * Finds what a shot would hit through a pixel of the view: the first voxel along that ray,
 * found by the game's own line of fire (TileEngine::calculateLineVoxel, the same 3D Bresenham
 * line through the same voxel shapes a real shot flies along). Walls, objects, floors and units
 * all stop it, except the soldier himself; units the player can't see are passed through
 * (unless the game is in debug mode), so the crosshair gives nothing away. Off into the sky,
 * it gives a point of the open air on the ray instead, inside the map (see skyReach), which a
 * shot may aim at as at any other: the game takes an empty tile as a target (its middle), and the
 * round flies on along the line past it.
 * @param pixelX Pixel column in the 3D view.
 * @param pixelY Pixel row in the 3D view.
 * @param unit The soldier looking (left out of the line).
 * @param voxel Set to the voxel hit, or to the point of open air on the ray.
 * @param voxelType If given, set to what was hit (V_FLOOR, V_WESTWALL, V_NORTHWALL, V_OBJECT,
 *   V_UNIT; V_EMPTY for nothing).
 * @return True if something was hit.
 */
bool FirstPersonMode::aimAt(int pixelX, int pixelY, BattleUnit *unit, Position &voxel, int *voxelType) const
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
	const int hitType = _save->getTileEngine()->calculateLineVoxel(rayOrigin, farPoint, false, &trajectory, unit, nullptr, !_save->getDebugMode());
	if (hitType != V_EMPTY && hitType != V_OUTOFBOUNDS && !trajectory.empty())
	{
		voxel = trajectory.front(); // a floor, a wall, an object or a unit was hit
		if (voxelType) *voxelType = hitType;
		return true;
	}
	if (voxelType) *voxelType = V_EMPTY;

	// Nothing hit (the sky, or out of the map): the point of the open air on the ray itself, as far
	// as skyReach, or the last one inside the map where the ray leaves it sooner. (A point beyond
	// the map, pulled back in one axis at a time, lay off to the side: a player's report, the shot
	// went "almost sideways" at the map's edge.)
	double reach = skyReach(unit);
	const double mapEnd[3] = { _save->getMapSizeX() * 16.0, _save->getMapSizeY() * 16.0, _save->getMapSizeZ() * 24.0 };
	for (int axis = 0; axis < 3; ++axis)
	{
		if (rayDirection[axis] > 1e-9) reach = std::min(reach, (mapEnd[axis] - 1.0 - eye[axis]) / rayDirection[axis]);
		else if (rayDirection[axis] < -1e-9) reach = std::min(reach, -eye[axis] / rayDirection[axis]);
	}
	reach = std::max(0.0, reach);
	voxel = Position((int)std::floor(eye[0] + rayDirection[0] * reach), (int)std::floor(eye[1] + rayDirection[1] * reach),
		(int)std::floor(eye[2] + rayDirection[2] * reach));
	// Steeply up near the top of the map the ray leaves it within the soldier's own tile, and a shot
	// at his own tile turned him to face south, and with the game's own aim shot his own floor (the
	// bug hunt after 184): the point a tile on along the ray's heading instead (straight up, the way
	// he faces), level with where the ray left the map. The map takes no target above its top, so
	// such a shot goes a little lower than the crosshair; its path shows it.
	if (unit)
	{
		const Position unitTile = unit->getPosition();
		const int size = unit->getArmor()->getSize();
		const Position pointTile = voxel.toTile();
		if (pointTile.x >= unitTile.x && pointTile.x < unitTile.x + size && pointTile.y >= unitTile.y && pointTile.y < unitTile.y + size)
		{
			double headingX = rayDirection[0], headingY = rayDirection[1];
			const double headingLength = std::sqrt(headingX * headingX + headingY * headingY);
			if (headingLength < 0.05)
			{
				Position facing;
				Pathfinding::directionToVector(unit->getDirection(), &facing);
				headingX = facing.x;
				headingY = facing.y;
			}
			const double out = 8.0 + 8.0 * size; // from the eye, past the soldier's tiles
			const double scale = out / std::max(0.05, std::sqrt(headingX * headingX + headingY * headingY));
			voxel.x = (int)std::floor(eye[0] + headingX * scale);
			voxel.y = (int)std::floor(eye[1] + headingY * scale);
		}
	}
	voxel.x = std::clamp<int>(voxel.x, 0, (int)mapEnd[0] - 1);
	voxel.y = std::clamp<int>(voxel.y, 0, (int)mapEnd[1] - 1);
	voxel.z = std::clamp<int>(voxel.z, 0, (int)mapEnd[2] - 1);
	return false;
}

/**
 * How far along the crosshair's ray a shot at the open sky aims (see aimAt): SkyAimReach, or less
 * for a weapon whose range ends sooner, a tile short of its end as the game counts tiles (it
 * refuses a shot at a tile beyond it: "Out of range"; a tile and a half, as a diagonal's tiles round
 * up), but a tile at least, so the point never
 * falls on the eye itself (a line with no length has no direction).
 * @param unit The soldier aiming (null: SkyAimReach).
 * @return The reach, in voxels.
 */
double FirstPersonMode::skyReach(BattleUnit *unit) const
{
	const BattleItem *weapon = unit ? activeWeapon(unit) : nullptr;
	if (!weapon)
	{
		return SkyAimReach;
	}
	return std::clamp((weapon->getRules()->getMaxRange() - 1.5) * 16.0, 16.0, SkyAimReach);
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
 * charges it. A melee blow turns the soldier the same way to the unit under the crosshair beside
 * them, or to the wall, door or object under it (see fire, turnBeforeUse). Turning by the rules
 * only: with free turning the soldier faces the target for free first (see fire). Nothing for
 * psi (no turn) or in Doom mode (time units don't run out).
 * @param unit The soldier aiming.
 * @param direction Set to the way the soldier (or its turret) will face for the shot.
 * @param affordable Set to whether the soldier has the time units for the turn and the shot.
 * @param melee If given, set to whether it's a melee blow's turn.
 * @return The time units the turn will cost; 0 for no turn.
 */
int FirstPersonMode::shotTurnCost(BattleUnit *unit, int &direction, bool &affordable, bool *melee) const
{
	direction = -1;
	affordable = true;
	if (melee) *melee = false;
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
	const BattleActionType mode = weapon ? _fireMode : BA_NONE;
	if (mode == BA_HIT)
	{
		// A melee blow: the turn to the unit under the crosshair beside the soldier, or to the
		// wall, door or object under it within reach, as fire takes them.
		const MeleeAim aim = meleeTarget(unit);
		Position struck;
		int part = V_EMPTY;
		if (aim.target)
		{
			direction = aim.direction;
		}
		else if (aim.refusal.empty() && meleeStrikesTerrain(weapon) && crosshairTerrainMelee(unit, struck, part, Options::fpsMeleeFloors))
		{
			direction = unit->directionTo(struck.toTile());
		}
		if (direction < 0 || direction == unit->getDirection())
		{
			direction = -1;
			return 0;
		}
		if (melee) *melee = true;
		const int eighths = std::min((direction - unit->getDirection() + 8) % 8, (unit->getDirection() - direction + 8) % 8);
		const int cost = eighths * unit->getTurnCost();
		affordable = cost + BattleActionCost(mode, unit, weapon).Time <= unit->getTimeUnits();
		return cost;
	}
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
 * A spray burst being marked (the game's "spray" auto shot, see fire), every frame: the points
 * marked so far and the next one, the tile under the crosshair, for the view to draw (see
 * FirstPersonView::drawSprayMarks); none while nothing is being marked. Each on its tile's floor,
 * in the middle, where the player clicked, as the map marks the tiles themselves (the game aims
 * the rounds at the middles of those tiles, waist high, which looking down at the ground nearby
 * would be out of the top of the view).
 * @param unit The soldier being looked through, or null.
 */
void FirstPersonMode::updateSprayMarks(BattleUnit *unit)
{
	const BattleAction *action = _state->_battleGame->getCurrentAction();
	auto onFloor = [this](Position tile) {
		tile.x = std::clamp<int>(tile.x, 0, _save->getMapSizeX() - 1);
		tile.y = std::clamp<int>(tile.y, 0, _save->getMapSizeY() - 1);
		tile.z = std::clamp<int>(tile.z, 0, _save->getMapSizeZ() - 1);
		const Tile *mapTile = _save->getTile(tile);
		return Position(tile.x * 16 + 8, tile.y * 16 + 8, tile.z * 24 - (mapTile ? mapTile->getTerrainLevel() : 0) + 1);
	};
	std::vector<Position> points;
	if (unit && isActive() && action->sprayTargeting && action->actor == unit)
	{
		for (const Position &tile : action->waypoints)
		{
			points.push_back(onFloor(tile));
		}
	}
	_view->setSprayMarks(points, onFloor(_aimVoxel.toTile()));
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
	// The game's own aim for a throw of it (Throw picked from the item's menu) ends here, as the
	// game ends its own throws: else the next click would throw the same grenade again, from
	// wherever it lies by then.
	BattleAction *gameAction = battleGame->getCurrentAction();
	if (gameAction->targeting && gameAction->weapon == grenade)
	{
		gameAction->targeting = false;
		gameAction->type = BA_NONE;
	}
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
	if (doomModeOn() && fuse != _doomFuses.end() && fuse->second.lit && now - fuse->second.lit >= fuse->second.length)
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
 * Both start from the side of the soldier the shot will leave from (leaning, see leanSide).
 * With dual wield the other gun's path shows too, from its muzzle, its own line or arc.
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
	const BattleActionType mode = weapon ? _fireMode : BA_NONE;

	// A marked target lasts while nothing it was marked with changes.
	if (_confirmTile.x >= 0 && (!Options::fpsConfirmFire || !playerTurn || unit != _confirmUnit || !unit || unit->getPosition() != _confirmFrom
		|| weapon != _confirmWeapon || mode != _confirmMode))
	{
		clearConfirmMark();
	}
	const bool marked = _confirmTile.x >= 0;

	// Dual wield: the other gun's path too, drawn from its muzzle (a player: "when using dual
	// fire, both weapons need laser's for trajectory predictions"): the gun that fires along (see
	// pairedGun), its own line or arc at the same target, as it will fire.
	BattleActionType offMode = BA_NONE;
	BattleItem *otherWeapon = weapon && isShot(mode) && weapon == activeWeapon(unit) ? pairedGun(unit, weapon, mode, offMode) : nullptr;

	// What to show, for a gun and its mode: an arc for a throw or an arcing shot, a line for a
	// straight shot (setting All, or a marked target); nothing while the soldier can't act.
	auto arcs = [&](BattleItem *gun, BattleActionType gunMode) { return gunMode == BA_THROW || (isShot(gunMode) && gun->getArcingShot(gunMode)); };
	auto shows = [&](BattleItem *gun, BattleActionType gunMode) {
		const bool arcing = arcs(gun, gunMode), straight = isShot(gunMode) && !arcing && (Options::fpsTrajectory >= 2 || marked);
		return isActive() && unit && canAct() && (Options::fpsTrajectory >= 1 || marked) && (arcing || straight);
	};
	const bool show = shows(weapon, mode);
	if (!otherWeapon || !shows(otherWeapon, offMode))
	{
		otherWeapon = nullptr;
	}
	if (!show)
	{
		if (!_trajectoryKey.empty())
		{
			_trajectoryKey.clear();
			_view->setTrajectory(std::vector<Position>());
			_view->setOtherTrajectory(std::vector<Position>());
		}
		return;
	}

	// The target: the marked one, or the tile and point under the crosshair (as fire() takes
	// them, see aimedTile).
	const Position targetTile = marked ? _confirmTile : aimedTile(_aimVoxel);
	const Position targetVoxel = marked ? _confirmVoxel : _aimVoxel;
	const bool hitSomething = marked || _aimHitsSomething;

	// The side of the soldier a shot leaves from, as fire() picks it: leaning (setting "Lean
	// shots", or the lean keys) moves it off the middle (see leanSide).
	const BattleActionOrigin side = (Options::fpsLeanShots || leaningOut()) && isShot(mode) ? leanSide(unit, targetTile) : BattleActionOrigin::CENTRE;

	// Worked out again only when something it depends on changed (a throw depends on the
	// target's tile only, a line and an arcing shot at an exact point on the very point; an
	// FPS-style line on where the eye is too, its rounds leaving from there, see freeAimFrom).
	const Position from = unit->getPosition();
	const bool fromEye = freeAimOn(mode) || (otherWeapon && freeAimOn(offMode));
	const int eyeVoxelX = fromEye ? (int)std::floor(_eyeX + _leanShift[0]) : 0, eyeVoxelY = fromEye ? (int)std::floor(_eyeY + _leanShift[1]) : 0;
	// (an arcing shot aims at the exact point as a straight one does, see launchShot; a throw at the tile)
	const bool arcing = arcs(weapon, mode);
	const bool exactArc = arcing && (Options::fpsAimExact || freeAimOn(mode)) && hitSomething && isShot(mode);
	const bool keyedPoint = !arcing || exactArc || (otherWeapon && !arcs(otherWeapon, offMode));
	const std::vector<int> key = { arcing ? 1 : 2, targetTile.x, targetTile.y, targetTile.z, keyedPoint ? targetVoxel.x : 0, keyedPoint ? targetVoxel.y : 0,
		keyedPoint ? targetVoxel.z : 0, from.x, from.y, from.z, unit->getDirection(), unit->isKneeled() ? 1 : 0, weapon->getId(), (int)mode, (int)side,
		Options::fpsForceArcing ? 1 : 0, Options::fpsForceBlocked ? 1 : 0, Options::oxceEnableOffCentreShooting ? 1 : 0, hitSomething ? 1 : 0,
		otherWeapon ? otherWeapon->getId() : -1, (int)offMode, eyeVoxelX, eyeVoxelY };
	if (key == _trajectoryKey)
	{
		return;
	}
	_trajectoryKey = key;

	// A gun's path, as the game will work it out for its action.
	TileEngine *tileEngine = _save->getTileEngine();
	auto pathOf = [&](BattleItem *gun, BattleActionType gunMode, std::vector<Position> &path) {
		BattleAction preview;
		preview.actor = unit;
		preview.weapon = gun;
		preview.type = gunMode;
		preview.target = targetTile;
		preview.relativeOrigin = side;
		const bool gunArcs = arcs(gun, gunMode);
		const bool gunExactArc = gunArcs && (Options::fpsAimExact || freeAimOn(gunMode)) && hitSomething && isShot(gunMode);
		if (gunExactArc) preview.targetVoxel = targetVoxel;
		FirstPersonView::TrajectoryLook look = FirstPersonView::TRAJECTORY_ARC;
		if (gunArcs)
		{
			// The game's own curve; or, for an arcing shot it would refuse, the forced one (setting
			// "Force arcing shots"), drawn yellow: it goes for the tile whatever it meets on the way.
			if (!gameArcFound(preview, targetTile, &path) && gunMode != BA_THROW && Options::fpsForceArcing && forcedArcFound(preview, targetTile, &path))
			{
				look = FirstPersonView::TRAJECTORY_FORCED;
			}
		}
		else
		{
			// The line of fire, up to what it meets, drawn as a laser sight: to the point under the
			// crosshair (FPS style; or with the game's own targeting, the line the shot takes, see
			// aimFor: to the point, to the game's own aim's, or forced, drawn yellow; or the one it
			// would take, dimmed, when the game will refuse it).
			// (FPS style: from the eye's place, as its rounds leave, see freeAimFrom)
			preview.freeAim = freeAimOn(gunMode);
			Position origin = tileEngine->getOriginVoxel(preview, _save->getTile(from)), voxel = targetVoxel;
			const ShotAim aim = freeAimOn(gunMode) ? AIM_EXACT : aimFor(preview, targetTile, targetVoxel, origin, voxel);
			tileEngine->calculateLineVoxel(origin, voxel, true, &path, unit);
			// (a shot the game will refuse shows dimmed: no surprise "No line of fire")
			look = aim == AIM_FORCED ? FirstPersonView::TRAJECTORY_FORCED_LINE : aim == AIM_REFUSED ? FirstPersonView::TRAJECTORY_REFUSED_LINE
				: FirstPersonView::TRAJECTORY_STRAIGHT;
		}
		return look;
	};
	std::vector<Position> path, otherPath;
	const FirstPersonView::TrajectoryLook look = pathOf(weapon, mode, path);
	const FirstPersonView::TrajectoryLook otherLook = otherWeapon ? pathOf(otherWeapon, offMode, otherPath) : FirstPersonView::TRAJECTORY_ARC;
	_view->setTrajectory(path, look);
	_view->setOtherTrajectory(otherPath, otherLook);
}

/**
 * The curve the game's own aim finds for a throw or an arcing shot at a tile, as the game works
 * it out (Projectile::calculateThrow) but without the accuracy roll: the points on the tile it
 * tries, in its order (an arcing shot aimed at an exact point, see launchShot, that point first;
 * then a unit's middle, low and high; an object, a wall, each at a few heights; the floor; a
 * throw always the floor), from the game's origin voxel (the hand), the first one it finds a
 * curve for that lands on the tile (TileEngine::validateThrow), and that curve followed to where
 * it comes down (calculateParabolaVoxel). At a wall seen from the side no curve lands on its
 * tile, and the game refuses the shot.
 * @param shot The action (actor, weapon, mode, the side it leaves from).
 * @param targetTile The tile aimed at.
 * @param path If given, set to the curve's voxels (left as it was when none is found).
 * @return True if the game finds a curve.
 */
bool FirstPersonMode::gameArcFound(BattleAction &shot, const Position &targetTile, std::vector<Position> *path)
{
	const Tile *tile = _save->getTile(targetTile);
	if (!tile)
	{
		return false;
	}
	const Position floorPoint = targetTile.toVoxel() + Position(8, 8, 1 - tile->getTerrainLevel());
	std::vector<Position> aimPoints;
	const BattleUnit *targetUnit = shot.type == BA_THROW ? nullptr : tile->getOverlappingUnit(_save);
	if (shot.type != BA_THROW && shot.targetVoxel.x >= 0)
	{
		aimPoints.push_back(shot.targetVoxel); // (an exact point aimed at: tried first)
	}
	if (shot.type == BA_THROW)
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

	TileEngine *tileEngine = _save->getTileEngine();
	const Position origin = tileEngine->getOriginVoxel(shot, nullptr);
	for (const Position &aimPoint : aimPoints)
	{
		double curvature = 0.0;
		if (tileEngine->validateThrow(shot, origin, aimPoint, _save->getDepth(), &curvature))
		{
			if (path) tileEngine->calculateParabolaVoxel(origin, aimPoint, true, path, shot.actor, curvature, Position(0, 0, 0));
			return true;
		}
	}
	return false;
}

/**
 * The curve of an arcing shot forced at a tile, as the game works it out with Ctrl held (force
 * fire, Projectile::calculateThrow): aimed at the exact point aimed at (see launchShot), or else
 * at one point of the tile, half a level up (the game takes the tile's corner voxel), a curve
 * accepted even where it meets something before it lands (TileEngine::validateThrow, forced),
 * followed to where it ends.
 * @param shot The action (actor, weapon, mode, the side it leaves from).
 * @param targetTile The tile aimed at.
 * @param path If given, set to the curve's voxels (left as it was when there's none).
 * @return True if there is a curve.
 */
bool FirstPersonMode::forcedArcFound(BattleAction &shot, const Position &targetTile, std::vector<Position> *path)
{
	TileEngine *tileEngine = _save->getTileEngine();
	const Position origin = tileEngine->getOriginVoxel(shot, nullptr);
	const Position aimPoint = shot.targetVoxel.x >= 0 ? shot.targetVoxel : targetTile.toVoxel() + Position(0, 0, 12);
	double curvature = 0.0;
	int meets = V_OUTOFBOUNDS;
	if (!tileEngine->validateThrow(shot, origin, aimPoint, _save->getDepth(), &curvature, &meets, true))
	{
		return false;
	}
	if (path) tileEngine->calculateParabolaVoxel(origin, aimPoint, true, path, shot.actor, curvature, Position(0, 0, 0));
	return true;
}

/**
 * Is this an arcing shot the game would refuse (its own aim finds no curve onto the tile, see
 * gameArcFound), which the setting "Force arcing shots" fires forced, as with Ctrl held (a
 * player's report: firing at the side of a building with a Panzerfaust, the game refused and
 * showed no path)? Where the game's own aim works, it's kept (its unit, object and wall points).
 * @param shot The action about to be fired.
 * @param targetTile The tile aimed at.
 * @return True if the shot should be forced.
 */
bool FirstPersonMode::forcesArc(BattleAction &shot, const Position &targetTile)
{
	if (!Options::fpsForceArcing || !isShot(shot.type) || !shot.weapon || !shot.weapon->getArcingShot(shot.type))
	{
		return false;
	}
	BattleAction probe = shot;
	return !gameArcFound(probe, targetTile, nullptr);
}

/**
 * The tile a point under the crosshair aims at: the tile it's in, kept inside the map; or, when
 * it's on a unit's head poking up into the tile above (a unit on stairs or raised ground), the
 * unit's own tile, as the game's cursor takes it (Tile::getOverlappingUnit). Aimed at the tile
 * above, the game found no unit there and refused even a shot that hit that very unit (a
 * player's report: "no line of fire" with the shot plainly on the unit).
 * @param aimedVoxel The point the aim met.
 * @return The tile aimed at.
 */
Position FirstPersonMode::aimedTile(const Position &aimedVoxel) const
{
	Position tile = aimedVoxel.toTile();
	tile.x = std::clamp<int>(tile.x, 0, _save->getMapSizeX() - 1);
	tile.y = std::clamp<int>(tile.y, 0, _save->getMapSizeY() - 1);
	tile.z = std::clamp<int>(tile.z, 0, _save->getMapSizeZ() - 1);
	const Tile *aimed = _save->getTile(tile);
	if (aimed && !aimed->getUnit() && aimed->getOverlappingUnit(_save))
	{
		--tile.z;
	}
	return tile;
}

namespace
{
/**
 * The game's own test of what a line of fire met first against the target (Projectile::
 * calculateTrajectory): the target tile, a wall on its near side, or the target's unit on another
 * of its tiles; a unit with no unit on the tile it was met in is one lower down (as the game says).
 * @param save The battle.
 * @param meets What the line met (V_FLOOR, V_UNIT...).
 * @param line The line's points, the first where it met it.
 * @param targetTile The tile aimed at.
 */
bool lineMetTarget(SavedBattleGame *save, int meets, const std::vector<Position> &line, const Position &targetTile)
{
	// The tile it meets first; a unit with no unit on that tile is one lower down (as the game says).
	Position hitTile = line.front().toTile();
	const Tile *hit = save->getTile(hitTile);
	if (meets == V_UNIT && hit && !hit->getUnit())
	{
		--hitTile.z;
		hit = save->getTile(hitTile);
	}
	if (hitTile == targetTile)
	{
		return true;
	}
	// Elsewhere: a wall on the near side of the target tile, or the target's unit on another
	// of its tiles (the game's own checks, as it makes them).
	if (meets == V_NORTHWALL) return hitTile.y - 1 == targetTile.y;
	if (meets == V_WESTWALL) return hitTile.x - 1 == targetTile.x;
	if (meets == V_UNIT)
	{
		const Tile *target = save->getTile(targetTile);
		return hit && target && hit->getUnit() == target->getUnit();
	}
	return false;
}
}

/**
 * Does the game let a straight shot fly along a line, as it checks the player's first round
 * (Projectile::calculateTrajectory)? The line must meet the target tile first: a unit on it (or
 * the same unit on another of its tiles), a wall on its side, anything in it; or meet nothing.
 * Whatever else it meets first, the game says "No line of fire".
 * @param shot The action (its actor is left out of the line).
 * @param origin Where the shot leaves from.
 * @param voxel The point it's aimed at.
 * @param targetTile The tile aimed at.
 * @return True if the game lets it fly.
 */
bool FirstPersonMode::gameLetsLineFly(BattleAction &shot, const Position &origin, const Position &voxel, const Position &targetTile)
{
	std::vector<Position> line;
	const int meets = _save->getTileEngine()->calculateLineVoxel(origin, voxel, false, &line, shot.actor);
	if (meets == V_EMPTY || line.empty())
	{
		return true;
	}
	return lineMetTarget(_save, meets, line, targetTile);
}

/**
 * Does a line from a muzzle point through a point near the crosshair's, carried on past it (three
 * tiles further), meet the shot's target first, as gameLetsLineFly counts it (the target tile, a
 * wall on its near side, or its unit)? Not meeting anything doesn't count here: the point is to
 * hit the target (see aimFor's search for a point near the crosshair's).
 * @param shot The action (its actor is left out of the line).
 * @param origin Where the shot leaves from.
 * @param point The point it would be aimed at.
 * @param targetTile The tile aimed at.
 * @param hit Set to where the line meets the target (when it does).
 * @return True if the line through it meets the target first.
 */
bool FirstPersonMode::lineThroughMeetsTarget(BattleAction &shot, const Position &origin, const Position &point, const Position &targetTile, Position &hit)
{
	const double dx = point.x - origin.x, dy = point.y - origin.y, dz = point.z - origin.z;
	const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (length < 1.0)
	{
		return false;
	}
	const double on = 48.0 / length; // three tiles past the point
	const Position beyond((int)std::lround(point.x + dx * on), (int)std::lround(point.y + dy * on), (int)std::lround(point.z + dz * on));
	std::vector<Position> line;
	const int meets = _save->getTileEngine()->calculateLineVoxel(origin, beyond, false, &line, shot.actor);
	if (meets == V_EMPTY || meets == V_OUTOFBOUNDS || line.empty() || !lineMetTarget(_save, meets, line, targetTile))
	{
		return false;
	}
	hit = line.front();
	return true;
}

/**
 * The game's own aim at a tile for a straight shot (ProjectileFlyBState::init, without the
 * accuracy roll), which a shot whose point under the crosshair has no line of fire falls back
 * on (see aimFor):
 * - at a unit the soldier sees: any part of it a line reaches (TileEngine::canTargetUnit), from
 *   the side the shot leaves from, then from the middle (a lean, see leanSide), then left and
 *   right of the middle with OXCE's "Off-centre shooting" (the game's own order); the side that
 *   works is kept in the action, as the game keeps it;
 * - at an object, a wall or a floor: a point on it a line reaches (canTargetTile), else its
 *   middle; at nothing, the tile's middle; and the game then lets the line fly or not
 *   (gameLetsLineFly).
 * @param shot The action (actor, weapon, mode, the side it leaves from).
 * @param targetTile The tile aimed at.
 * @param origin Set to where the shot leaves from.
 * @param voxel Set to the point aimed at.
 * @return True if the game finds a line of fire.
 */
bool FirstPersonMode::gameAimFound(BattleAction &shot, const Position &targetTile, Position &origin, Position &voxel)
{
	TileEngine *tileEngine = _save->getTileEngine();
	Tile *tile = _save->getTile(targetTile);
	Tile *from = _save->getTile(shot.actor->getPosition());
	if (!tile || !from)
	{
		return false;
	}
	const BattleUnit *targetUnit = tile->getUnit();
	if (targetUnit && targetUnit != shot.actor && targetUnit->getVisible())
	{
		std::vector<BattleActionOrigin> sides = { shot.relativeOrigin };
		if (shot.relativeOrigin != BattleActionOrigin::CENTRE) sides.push_back(BattleActionOrigin::CENTRE);
		if (Options::oxceEnableOffCentreShooting)
		{
			sides.push_back(BattleActionOrigin::LEFT);
			sides.push_back(BattleActionOrigin::RIGHT);
		}
		for (const BattleActionOrigin side : sides)
		{
			shot.relativeOrigin = side;
			origin = tileEngine->getOriginVoxel(shot, from);
			if (tileEngine->canTargetUnit(&origin, tile, &voxel, shot.actor, false))
			{
				return true;
			}
		}
		shot.relativeOrigin = BattleActionOrigin::CENTRE;
		return false;
	}
	origin = tileEngine->getOriginVoxel(shot, from);
	const Position corner = targetTile.toVoxel();
	if (tile->getMapData(O_OBJECT))
	{
		if (!tileEngine->canTargetTile(&origin, tile, O_OBJECT, &voxel, shot.actor, false)) voxel = corner + Position(8, 8, 10);
	}
	else if (tile->getMapData(O_NORTHWALL))
	{
		if (!tileEngine->canTargetTile(&origin, tile, O_NORTHWALL, &voxel, shot.actor, false)) voxel = corner + Position(8, 0, 9);
	}
	else if (tile->getMapData(O_WESTWALL))
	{
		if (!tileEngine->canTargetTile(&origin, tile, O_WESTWALL, &voxel, shot.actor, false)) voxel = corner + Position(0, 8, 9);
	}
	else if (tile->getMapData(O_FLOOR))
	{
		if (!tileEngine->canTargetTile(&origin, tile, O_FLOOR, &voxel, shot.actor, false)) voxel = corner + Position(8, 8, 2);
	}
	else
	{
		voxel = corner + TileEngine::voxelTileCenter;
	}
	return gameLetsLineFly(shot, origin, voxel, targetTile);
}

/**
 * How a straight shot with the game's own targeting (setting "Faithful targeting") goes, in
 * order (a player's report: "no line of fire" with the shot plainly on the unit; the owner's
 * choices):
 * - at the point under the crosshair (setting "Aim at exact point"; on the open sky, the point of
 *   air on its ray, see aimAt), if the game lets that line fly (gameLetsLineFly): from the side
 *   of the shot, then from the middle, then left and right of it with OXCE's "Off-centre
 *   shooting", the game's own muzzle points in the order its own aim tries them for a unit (a
 *   player's report: past the very corner of a craft's opening the shot went for the hull); the
 *   side that works is left in the action, for the game to fire from;
 * - else at the nearest point around it, within NearPointReach voxels, that a line through
 *   reaches the same target (upwards first: over a ledge, off a bumpy floor), where it meets it
 *   seen from the eye no further off the crosshair than NearPointReach and a half at the
 *   crosshair's point (along a floor it lands a little behind, on the same line of sight; a line
 *   slipping past a corner to the far side of the target tile doesn't count), from the same
 *   muzzle points (a player's report, with screenshots: shots "very visually off", lasers along
 *   flat ground jutting off to a side; the game's own aim below went for another part of the
 *   unit, or the tile's middle);
 * - else as the game's own aim goes: any part of the unit a line reaches, from the side of the
 *   shot, the middle, and left and right with OXCE's "Off-centre shooting" (gameAimFound);
 * - else, with "Force blocked shots", forced along the line to the crosshair, hitting whatever
 *   is in the way, as with Ctrl held;
 * - else the game refuses it ("No line of fire").
 * @param shot The action (actor, weapon, mode, the side it leaves from; left as it was, but for
 *   an exact shot the side its line flies from).
 * @param targetTile The tile aimed at.
 * @param aimedVoxel The point under the crosshair.
 * @param origin Set to where the shot leaves from (for its path).
 * @param voxel Set to the point it goes for (for its path).
 * @return How the shot goes.
 */
FirstPersonMode::ShotAim FirstPersonMode::aimFor(BattleAction &shot, const Position &targetTile, const Position &aimedVoxel, Position &origin, Position &voxel)
{
	TileEngine *tileEngine = _save->getTileEngine();
	Tile *from = _save->getTile(shot.actor->getPosition());
	const BattleActionOrigin side = shot.relativeOrigin;
	origin = tileEngine->getOriginVoxel(shot, from);
	voxel = aimedVoxel;
	if (Options::fpsAimExact)
	{
		// The exact point from each muzzle point the game may fire from, in its order.
		std::vector<BattleActionOrigin> sides = { side };
		if (side != BattleActionOrigin::CENTRE) sides.push_back(BattleActionOrigin::CENTRE);
		if (Options::oxceEnableOffCentreShooting)
		{
			for (const BattleActionOrigin offCentre : { BattleActionOrigin::LEFT, BattleActionOrigin::RIGHT })
			{
				if (offCentre != side) sides.push_back(offCentre);
			}
		}
		for (const BattleActionOrigin tried : sides)
		{
			shot.relativeOrigin = tried;
			const Position triedOrigin = tileEngine->getOriginVoxel(shot, from);
			if (gameLetsLineFly(shot, triedOrigin, aimedVoxel, targetTile))
			{
				origin = triedOrigin;
				return AIM_EXACT;
			}
		}
		// No line to it: the nearest point around it a line through reaches the target, the
		// points in order of their distance (upwards first at the same distance), at most
		// NearPointMostTries lines in all (each point from each muzzle point counts one).
		static const std::vector<Position> nearOffsets = []
		{
			std::vector<Position> offsets;
			for (int z = -NearPointReach; z <= NearPointReach; ++z)
				for (int y = -NearPointReach; y <= NearPointReach; ++y)
					for (int x = -NearPointReach; x <= NearPointReach; ++x)
						if ((x || y || z) && x * x + y * y + z * z <= NearPointReach * NearPointReach) offsets.push_back(Position(x, y, z));
			std::stable_sort(offsets.begin(), offsets.end(), [](const Position &a, const Position &b)
			{
				const int distanceA = a.x * a.x + a.y * a.y + a.z * a.z, distanceB = b.x * b.x + b.y * b.y + b.z * b.z;
				return distanceA != distanceB ? distanceA < distanceB : a.z > b.z;
			});
			return offsets;
		}();
		const int mapX = _save->getMapSizeX() * 16, mapY = _save->getMapSizeY() * 16, mapZ = _save->getMapSizeZ() * 24;
		// How far off the crosshair (seen from the eye) the round may land: the angle of
		// NearPointReach and a half at the crosshair's point, as a cosine.
		double eye[3];
		_view->getEye(eye);
		const double toAimed[3] = { aimedVoxel.x - eye[0], aimedVoxel.y - eye[1], aimedVoxel.z - eye[2] };
		const double aimedDistance = std::max(1.0, std::sqrt(toAimed[0] * toAimed[0] + toAimed[1] * toAimed[1] + toAimed[2] * toAimed[2]));
		const double leastCosine = std::cos(std::atan(NearPointReach * 1.5 / aimedDistance));
		auto onTheCrosshair = [&](const Position &hit)
		{
			const double toHit[3] = { hit.x - eye[0], hit.y - eye[1], hit.z - eye[2] };
			const double hitDistance = std::sqrt(toHit[0] * toHit[0] + toHit[1] * toHit[1] + toHit[2] * toHit[2]);
			return hitDistance > 0.0 && (toHit[0] * toAimed[0] + toHit[1] * toAimed[1] + toHit[2] * toAimed[2]) / (hitDistance * aimedDistance) >= leastCosine;
		};
		int lines = 0;
		for (const Position &offset : nearOffsets)
		{
			const Position near = aimedVoxel + offset;
			if (near.x < 0 || near.y < 0 || near.z < 0 || near.x >= mapX || near.y >= mapY || near.z >= mapZ)
			{
				continue;
			}
			for (const BattleActionOrigin tried : sides)
			{
				if (++lines > NearPointMostTries) break;
				shot.relativeOrigin = tried;
				const Position triedOrigin = tileEngine->getOriginVoxel(shot, from);
				Position hit;
				if (gameLetsLineFly(shot, triedOrigin, near, targetTile) && lineThroughMeetsTarget(shot, triedOrigin, near, targetTile, hit) && onTheCrosshair(hit))
				{
					origin = triedOrigin;
					voxel = near;
					return AIM_EXACT;
				}
			}
			if (lines > NearPointMostTries) break;
		}
		shot.relativeOrigin = side;
	}
	Position gameOrigin, gameVoxel;
	const bool gameFound = gameAimFound(shot, targetTile, gameOrigin, gameVoxel);
	shot.relativeOrigin = side;
	if (gameFound)
	{
		origin = gameOrigin;
		voxel = gameVoxel;
		return AIM_GAME;
	}
	return Options::fpsForceBlocked ? AIM_FORCED : AIM_REFUSED;
}

/**
 * Uses the fire mode on whatever is under a pixel of the view (the crosshair with mouse look,
 * the cursor otherwise). Everything goes through the game's own actions, exactly as a click
 * on the isometric map would: the game charges the time units, rolls the accuracy (in FPS
 * style the spread takes the roll's place, see below), flies the shot along its line of fire
 * and triggers reaction fire.
 * - Shots and throws target the tile under the pixel (or its exact voxel, see below).
 * - A guided launcher and psi on a creature out of sight are aimed on the isometric map.
 * - Melee strikes the unit beside the soldier under the pixel (turning to it first), the wall,
 *   door or object under it within reach, or with nothing of these under it whatever is in
 *   front of the soldier.
 * - Medi-kits and scanners are used at once; priming opens the game's timer box straight away
 *   (the fire mode already says what the item's action menu would ask again); a psi amp's or a
 *   mind probe's use targets as psi does.
 * @param pixelX Pixel column in the 3D view.
 * @param pixelY Pixel row in the 3D view.
 */
void FirstPersonMode::fire(int pixelX, int pixelY)
{
	// Only a soldier of ours who may act now.
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canAct() || !_state->playableUnitSelected() || _turnShot.weapon)
	{
		return; // (nor while the other gun's shot waits to fire in turn, see fireTurnShot)
	}
	updateFireMode(unit);
	BattleItem *weapon = activeWeapon(unit);
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();
	BattleActionType mode = _fireMode;

	// A spray burst being marked (the game's "spray" auto shot, a player: "Ctrl shift for spray
	// waypoints ... needs first person equivalent"): begun with Ctrl+Shift and a click (launchShot
	// hands that to the game, which starts marking), each click marks the next point, the tile
	// under the crosshair, the game's own way: the one that reaches the weapon's number of points
	// (or one with Ctrl and Shift held) fires the burst along them. The right button takes the
	// last point back (the game's cancel). See updateSprayMarks.
	if (action->sprayTargeting && action->actor == unit)
	{
		Position aimed;
		aimAt(pixelX, pixelY, unit, aimed);
		Position tile = aimed.toTile();
		tile.x = std::clamp<int>(tile.x, 0, _save->getMapSizeX() - 1);
		tile.y = std::clamp<int>(tile.y, 0, _save->getMapSizeY() - 1);
		tile.z = std::clamp<int>(tile.z, 0, _save->getMapSizeZ() - 1);
		// (Free turning, no "Turning costs TU": the soldier faces the first point for free, so the
		// game's turn to it before the burst costs nothing, as for any shot, see launchShot.)
		if (!Options::fpsTurnCost && !action->waypoints.empty() && unit->directionTo(action->waypoints.front()) != unit->getDirection())
		{
			unit->setDirection(unit->directionTo(action->waypoints.front()));
			_save->getTileEngine()->calculateFOV(unit);
			_save->getTileEngine()->calculateFOV(unit->getPosition(), 1, false); // others' view of this unit, like UnitTurnBState
		}
		battleGame->primaryAction(tile);
		_triggerHeld = false;
		return;
	}

	// An action picked from an item's menu that wants a target (the mind probe, psi picked from
	// the menu...): this click is its target, with that item and action. Not a shot, though:
	// after a shot the game keeps aiming that same shot (so the player can fire again), and a
	// shot picked on the classic HUD has just been taken over as the fire mode (updateFireMode),
	// so a shot fires as the fire mode does, the other hand's gun joining in with dual wield.
	const bool menuTargeting = action->targeting && action->actor == unit && action->weapon && !isShot(action->type);
	if (menuTargeting)
	{
		// (An item the soldier no longer carries, thrown or dropped since: that aim is over.)
		if (action->weapon->getOwner() != unit)
		{
			action->targeting = false;
			action->type = BA_NONE;
			return;
		}
		weapon = action->weapon;
		mode = action->type;
	}
	if (!weapon || mode == BA_NONE)
	{
		return;
	}

	// The game's own checks before any use but a throw, as its action menu makes them: an alien
	// artifact the base hasn't researched yet, or a weapon this soldier can't use here (under
	// water, a mod's limits). Refused, a held trigger counts as let go.
	if (mode != BA_THROW)
	{
		std::string refusal = "STR_UNKNOWN"; // (the game's fall-back when it gives no reason)
		if (unit->getOriginalFaction() == FACTION_PLAYER && !_game->getSavedGame()->isResearched(weapon->getRules()->getRequirements()))
		{
			refusal = "STR_UNABLE_TO_USE_ALIEN_ARTIFACT_UNTIL_RESEARCHED";
		}
		else if (_save->canUseWeapon(weapon, unit, false, mode, &refusal))
		{
			refusal.clear();
		}
		if (!refusal.empty())
		{
			_state->warning(refusal);
			_triggerHeld = false;
			return;
		}
	}

	// A guided missile's waypoints are placed on the isometric map, by the game's own clicks.
	if (mode == BA_LAUNCH)
	{
		startMapAim(unit, weapon, BA_LAUNCH, "STR_FPS_LAUNCH_HINT");
		return;
	}

	// Psi and the mind probe reach creatures out of sight: with no creature under the pixel,
	// the player picks one on the isometric map instead. (A psi amp's or a mind probe's use,
	// picked as the fire mode or from the item's menu, the same way.)
	const BattleType itemType = weapon->getRules()->getBattleType();
	const bool psiUse = mode == BA_USE && (itemType == BT_MINDPROBE || itemType == BT_PSIAMP);
	if (mode == BA_PANIC || mode == BA_MINDCONTROL || psiUse)
	{
		Position pointedVoxel;
		aimAt(pixelX, pixelY, unit, pointedVoxel);
		if (!creatureAt(pointedVoxel, unit))
		{
			startMapAim(unit, weapon, mode, "STR_FPS_PSI_HINT");
			return;
		}
	}

	// Doom mode's real-time grenades (always with Doom mode; a first step towards a real-time mode). PRIME
	// opens the fuse box (1 to DoomFuseMaxSeconds seconds), as the isometric map's priming opens
	// its timer; the seconds picked are kept (setting "Doom grenade fuse") for every grenade after.
	// - Prime on throw (setting "Doom prime on throw", the default): that's all; the grenade is
	//   armed as it's thrown (below) and goes off that many seconds after it leaves the hand.
	// - Otherwise the pick pulls the pin, with its click, and the fuse burns from then on, in the
	//   hand too ("cooking"): throw it, or don't.
	if (mode == BA_PRIME && doomModeOn() && !menuTargeting && timedGrenade(weapon))
	{
		_game->pushState(new DoomFuseState([this, unit, weapon](int seconds)
		{
			Options::fpsDoomGrenadeFuse = seconds;
			Options::save(); // (kept, so the options screen's Cancel doesn't quietly undo it)
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

	// A medi-kit or a scanner: used at once, no menu (see useItemDirectly).
	if (mode == BA_USE && !menuTargeting && (itemType == BT_MEDIKIT || itemType == BT_SCANNER))
	{
		useItemDirectly(unit, weapon);
		return;
	}
	// Priming: what the game's action menu does once PRIME is picked, without the menu asking
	// again what the fire mode already says: the game's timer box for a grenade whose timer is
	// set (PrimeGrenadeState), else the item's own fuse at once. The game then primes it as
	// after its menu (its time units, message and sound; nothing if the box is cancelled).
	if (mode == BA_PRIME && !menuTargeting)
	{
		action->actor = unit;
		action->weapon = weapon;
		action->type = BA_PRIME;
		action->targeting = false;
		action->result.clear();
		action->value = -1;
		action->updateTU();
		if (weapon->getRules()->getFuseTimerType() == BFT_SET)
		{
			_state->popup(new PrimeTimerHost(action)); // (the box closes what's under it, see PrimeTimerHost)
		}
		else
		{
			action->value = weapon->getRules()->getFuseTimerDefault();
			battleGame->handleNonTargetAction();
		}
		return;
	}
	// Any other use without a target of its own: the item's action menu, as clicking it on the
	// HUD. (A psi amp's or a mind probe's use goes on below, as a targeted action.)
	if (mode == BA_USE && !menuTargeting && !psiUse)
	{
		_state->handleItemClick(weapon, false);
		return;
	}

	// Melee, by the crosshair first (a player's report: "Melee locked to compass facing"): a unit
	// standing under it beside the soldier, who turns to face it first (see meleeTarget; one
	// under it farther off is out of range); else the wall, door or object under it within
	// reach, diagonal or a level up or down too (see crosshairTerrainMelee; the map's own strikes
	// only the one straight ahead), turning to its tile, if the blow may strike terrain (the
	// mod's terrain melee, extendedTerrainMelee, or the setting "Melee hits walls and the
	// downed", see meleeStrikesTerrain), and nothing if it may not; with terrain melee and the
	// setting "Melee hits floors", the floor or ceiling under it too (see meleeFloor); a unit
	// straight above or below the game's line reaches (see meleeTarget); with the crosshair on none of
	// these (open floor, the sky, out of reach), as the game does it: who is in reach the way the
	// soldier faces, then (that setting) a body lying in front (see meleeDowned). The turn goes
	// by the rules or is free (turnBeforeUse), and the game does the attack as its own action,
	// which needs no target click.
	if (mode == BA_HIT)
	{
		TileEngine *tileEngine = _save->getTileEngine();
		Position meleeTile, struck;
		int part = V_EMPTY;
		action->actor = unit;
		action->weapon = weapon;
		action->terrainMeleeTilePart = 0;
		const MeleeAim aim = meleeTarget(unit);
		int turnTo = -1;
		if (!aim.refusal.empty())
		{
			_state->warning(aim.refusal);
			return;
		}
		if (aim.target)
		{
			action->target = aim.tile;
			turnTo = aim.direction;
		}
		else if (crosshairTerrainMelee(unit, struck, part, Options::fpsMeleeFloors && meleeStrikesTerrain(weapon)))
		{
			if (!meleeStrikesTerrain(weapon))
			{
				_state->warning("STR_THERE_IS_NO_ONE_THERE");
				return;
			}
			if (part == V_FLOOR)
			{
				meleeFloor(unit, weapon, action, struck);
				return;
			}
			action->target = struck.toTile();
			action->terrainMeleeTilePart = part;
			action->targetVoxel = struck; // (the hit lands there: MeleeAttackBState)
			turnTo = unit->directionTo(action->target); // (its own tile's wall: no turn)
		}
		else if (tileEngine->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &meleeTile))
		{
			action->target = meleeTile;
		}
		else if (Options::fpsMeleeTerrain && meleeDowned(unit, weapon, action))
		{
			return;
		}
		else
		{
			_state->warning("STR_THERE_IS_NO_ONE_THERE");
			return;
		}
		action->type = BA_HIT;
		action->targeting = false;
		action->result.clear();
		action->updateTU();
		const bool timeUnitsEnough = action->haveTU();
		if (timeUnitsEnough && !turnBeforeUse(unit, turnTo, action->Time))
		{
			action->type = BA_NONE;
			action->terrainMeleeTilePart = 0;
			action->targetVoxel = Position(-1, -1, -1);
			return;
		}
		battleGame->handleNonTargetAction();
		action->targetVoxel = Position(-1, -1, -1); // (taken by the game's melee, if it went ahead)

		// The weapon in hand swings only if the game went ahead. Refused for time units, the
		// game says so and nothing swings, and the melee isn't left waiting (as the game's
		// action menu clears it).
		if (!timeUnitsEnough)
		{
			action->type = BA_NONE;
		}
		else if (action->result.empty())
		{
			_swingStart = SDL_GetTicks();
			_swingJolted = false;
			if (Options::fpsHitSounds) _sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f); // (setting "Hit, kill and swing sounds")
		}
		return;
	}

	// Shots and throws from here on. An empty gun clicks.
	if (isShot(mode) && !weapon->getAmmoForAction(mode))
	{
		_sounds->play(FirstPersonSounds::DRY_FIRE); // the game refuses the shot itself
	}

	// The target tile: the one holding the voxel under the pixel (on the open sky the point of
	// air on the ray, inside the map, see aimAt).
	Position aimedVoxel;
	const bool hitSomething = aimAt(pixelX, pixelY, unit, aimedVoxel);
	const Position targetTile = aimedTile(aimedVoxel);

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
	if (mode == BA_THROW && doomModeOn() && Options::fpsDoomPrimeOnThrow && timedGrenade(weapon)
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

	// Dual wield (an option): the gun in the other hand fires too, both less accurate
	// (DualWieldAccuracy): the two take turns round by round, or fire as a volley (see
	// launchShot). With time units for only one of them, just the chosen hand fires, at full
	// accuracy (see pairedGun). (Not for an action picked from a menu.) With an arcing shot,
	// either gun's (a grenade launcher's, a Panzerfaust's), they fire gun after gun instead: this
	// gun now, alone and at full accuracy, the other once this shot has landed (see
	// fireTurnShot), each its own arc or line at the target (a player's report on 1.03: with two
	// grenade launchers the other one never fired, and this one was half as accurate for it).
	BattleActionType offMode = BA_NONE;
	BattleItem *otherWeapon = menuTargeting ? nullptr : pairedGun(unit, weapon, mode, offMode);
	const bool inTurn = otherWeapon && firesInTurn(weapon, mode, otherWeapon, offMode);
	if (otherWeapon && inTurn)
	{
		_turnShot = TurnShot{ unit, otherWeapon, offMode, targetTile, aimedVoxel, hitSomething };
		otherWeapon = nullptr;
	}
	launchShot(unit, weapon, mode, targetTile, aimedVoxel, hitSomething, otherWeapon, offMode);
}

/**
 * Dual wield: the gun in the other hand that fires along with a shot, if any: dualPartner's,
 * when there are time units for both (the more expensive gun's shot and a tenth more, see
 * dualWieldCharge; an arcing pair, fired gun after gun, each its own; in Doom mode always).
 * fire() fires it, and the trajectory shows its path too.
 * @param unit The soldier.
 * @param weapon The weapon in the chosen hand.
 * @param mode The fire mode picked for it.
 * @param offMode Set to the fire mode the other gun fires.
 * @return The other hand's gun, or null if it doesn't fire along.
 */
BattleItem *FirstPersonMode::pairedGun(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, BattleActionType &offMode) const
{
	BattleItem *otherWeapon = dualPartner(unit, weapon, mode, offMode);
	if (otherWeapon && !doomModeOn())
	{
		const bool inTurn = firesInTurn(weapon, mode, otherWeapon, offMode);
		const int chosenTime = BattleActionCost(mode, unit, weapon).Time, otherTime = BattleActionCost(offMode, unit, otherWeapon).Time;
		if (chosenTime + (inTurn ? otherTime : FirstPerson::dualWieldCharge(chosenTime, otherTime)) > unit->getTimeUnits())
		{
			otherWeapon = nullptr;
		}
	}
	return otherWeapon;
}

/**
 * Fires a shot or a throw at a tile through the game's own action, as a click on the isometric
 * map would (see fire): the game turns the soldier (by the rules), checks and spends the time
 * units, and launches it, with what the add-on adds to the action (FPS style's free aim, the
 * exact point, the side of a lean, a forced arc, the other gun firing along).
 * @param unit The soldier.
 * @param weapon The weapon fired or thrown.
 * @param mode The action (a shot or a throw).
 * @param targetTile The tile aimed at.
 * @param aimedVoxel The voxel the aim met there.
 * @param hitSomething Whether the aim met something (not the sky).
 * @param otherWeapon The gun in the other hand firing along (dual wield), or null.
 * @param otherMode That gun's fire mode.
 */
void FirstPersonMode::launchShot(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const Position &targetTile, const Position &aimedVoxel, bool hitSomething, BattleItem *otherWeapon, BattleActionType otherMode)
{
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();

	// Set up the game's current action as its own targeting would, and let it work out the
	// cost in time units.
	action->actor = unit;
	action->weapon = weapon;
	action->type = mode;
	action->targeting = true;
	action->waypoints.clear();
	action->updateTU();

	// Things the add-on adds to the action, read by the game's own shot code
	// (ProjectileFlyBState, Projectile; and forced, below):
	// - freeAim (FPS style): the game asks for each round's target the moment it fires it
	//   (freeAimTarget) and rolls no accuracy; the line of fire is still the game's.
	// - targetVoxel (setting "Aim at exact point", or FPS style): aim at the very voxel under
	//   the crosshair instead of the point the game picks on the target tile (the middle of a
	//   unit, an object...), or a straight shot on the open sky at the point of air on the
	//   crosshair's ray (see aimAt); the game's accuracy roll still applies to it.
	action->freeAim = freeAimOn(mode);
	if ((Options::fpsAimExact || action->freeAim) && (hitSomething || !weapon->getArcingShot(mode)) && isShot(mode))
	{
		action->targetVoxel = aimedVoxel;
	}
	// Leaning (setting "Lean shots", or leaning out with the lean keys): a soldier whose eye is
	// to one side of the tile's middle shoots from that side (see leanSide).
	action->relativeOrigin = (Options::fpsLeanShots || leaningOut()) && isShot(mode) ? leanSide(unit, targetTile) : BattleActionOrigin::CENTRE;
	// An arcing shot the game would refuse fires forced, as with Ctrl held (setting "Force
	// arcing shots", see forcesArc).
	action->forced = forcesArc(*action, targetTile);
	// A straight shot with the game's own targeting: at the exact point if its line flies (from
	// the muzzle point that lets it, which the game then fires from); else the game's own aim (the
	// exact point dropped: the game then aims at any part of the unit, off-centre too, as it does
	// on the map); else forced along the line to the crosshair ("Force blocked shots"); else as it
	// was, refused (see aimFor).
	if (!action->freeAim && isShot(mode) && !weapon->getArcingShot(mode))
	{
		BattleAction probe = *action;
		probe.target = targetTile; // (the game sets it as it fires; the shot's origin depends on it)
		Position origin, voxel;
		switch (aimFor(probe, targetTile, aimedVoxel, origin, voxel))
		{
		case AIM_EXACT:
			action->relativeOrigin = probe.relativeOrigin;
			action->targetVoxel = voxel; // (the point under the crosshair, or the nearest one a line reaches, see aimFor)
			break;
		case AIM_GAME:
			action->targetVoxel = Position(-1, -1, -1);
			break;
		case AIM_FORCED:
			action->targetVoxel = aimedVoxel;
			action->forced = true;
			break;
		default:
			break;
		}
	}

	// Dual wield: the two guns take turns round by round, each round resolved before the next
	// leaves (ProjectileFlyBState::takeTurn), or with "Akimbo volley" fire both at once; either way
	// both less accurate (the game cuts both shots' accuracy, see DualWieldAccuracy) and for the
	// pair's cost (dualWieldCharge), if the caller found one to fire.
	action->dualWield = otherWeapon != nullptr;
	action->dualVolley = otherWeapon != nullptr && Options::fpsAkimboVolley;
	action->dualWeapon = otherWeapon;
	action->dualType = otherWeapon ? otherMode : BA_NONE;

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
	// (Ctrl and Shift held with a spray weapon's auto shot: the game starts marking a spray burst
	// instead, its first point this tile; the trigger is let go, so FPS style's held auto doesn't
	// mark more, and confirm fire's second click isn't made: it would fire the burst at once.)
	battleGame->primaryAction(targetTile);
	if (action->sprayTargeting)
	{
		_triggerHeld = false;
	}
	else if (Options::battleConfirmFireMode && action->targeting && !action->waypoints.empty())
	{
		battleGame->primaryAction(targetTile);
	}
	_shotAimLeft = isShot(mode); // the game keeps aiming the shot after it: see endShotAim

	// Clear what we added, so the next action (one from the classic HUD, say) aims normally.
	action->targetVoxel = Position(-1, -1, -1);
	action->freeAim = false;
	action->forced = false;
	action->relativeOrigin = BattleActionOrigin::CENTRE;
	action->dualWield = false;
	action->dualVolley = false;
	action->dualWeapon = nullptr;
	action->dualType = BA_NONE;
}

/**
 * Akimbo: do the two guns fire in turn rather than together? When either one's shot arcs: the
 * other gun's round could neither follow an arc nor fire one beside the first (see fire).
 * @param weapon The chosen gun.
 * @param mode Its fire mode (as fired).
 * @param otherWeapon The gun in the other hand.
 * @param otherMode Its fire mode.
 * @return True if they fire in turn.
 */
bool FirstPersonMode::firesInTurn(const BattleItem *weapon, BattleActionType mode, const BattleItem *otherWeapon, BattleActionType otherMode) const
{
	return weapon->getArcingShot(mode) || otherWeapon->getArcingShot(otherMode);
}

/**
 * An arcing pair's turn (see fire), every frame: the other gun's shot, once the game is done with the
 * first (idle again: the shot landed, its blast and any reaction fire over), at the same target,
 * at full accuracy, leaving from that gun's muzzle on the view. Called off if the soldier is no
 * longer the one selected or is down, the 3D view was left, or the gun is no longer in hand or
 * loaded; with too few time units left the game refuses it, as it would a click.
 */
void FirstPersonMode::fireTurnShot()
{
	if (!_turnShot.weapon)
	{
		return;
	}
	BattleUnit *unit = _save->getSelectedUnit();
	if (!isActive() || unit != _turnShot.unit || unit->isOut())
	{
		_turnShot = TurnShot();
		return;
	}
	if (!canAct() || _state->_battleGame->isBusy())
	{
		return; // the first shot still on its way, or what follows it
	}
	const TurnShot shot = _turnShot;
	_turnShot = TurnShot();
	if (shot.weapon->getOwner() != unit || !shot.weapon->getAmmoForAction(shot.mode))
	{
		return;
	}
	_view->setNextShotFromOffHand(true);
	launchShot(unit, shot.weapon, shot.mode, shot.targetTile, shot.aimedVoxel, shot.hitSomething, nullptr, BA_NONE);
	if (_state->_battleGame->isBusy())
	{
		_turnShotFired = shot.weapon;
	}
	else
	{
		_view->setNextShotFromOffHand(false); // refused (time units): the next shot is this gun's again
	}
}

/**
 * Whom a medi-kit would treat now, by the crosshair (a player: "the same targetting system ...
 * proposed for melee"), with the game's own rules for whom the kit may treat (standing or lying,
 * friend, neutral or foe, immune to wounds or not):
 * - a unit standing under the crosshair (or the free cursor: its hitbox or picture, see
 *   updatePointedUnit): treated if it's on a tile next to
 *   the soldier (any of the eight ways, by the game's melee reach, TileEngine::validMeleeRange),
 *   the soldier first turning to face it (the way needing the fewest eighths); farther off, out
 *   of range (none, either way, if the kit can't treat it);
 * - a unit lying unconscious under the crosshair (its body's picture, see
 *   FirstPersonView::getBodyAtPointer: a body lying flat reaches far towards the camera, so not
 *   where the line of fire meets the floor): treated if it lies at the soldier's feet (as the
 *   game treats bodies: on the soldier's own tile only); elsewhere, the player is told to stand
 *   over it;
 * - looking down (more than 25 degrees) with a body at the feet: that body;
 * - aiming at nothing of these: the soldier themself;
 * - and when the kit can't treat the soldier, the game's own order: the body at the feet, then
 *   the unit in the tile faced.
 * @param unit The soldier with the kit.
 * @param medikit The kit.
 * @return The patient (null if none, with the reason), and the way to turn to them.
 */
FirstPersonMode::MedikitAim FirstPersonMode::medikitPatient(BattleUnit *unit, BattleItem *medikit) const
{
	MedikitAim aim;
	const RuleItem *rules = medikit->getRules();
	TileEngine *tileEngine = _save->getTileEngine();
	// Whether the kit may treat a unit of this side, standing or lying.
	auto sideAllowed = [rules](const BattleUnit *patient, bool lying) {
		const UnitFaction faction = patient->getOriginalFaction();
		return lying ? ((rules->getAllowTargetFriendGround() && faction == FACTION_PLAYER) || (rules->getAllowTargetNeutralGround() && faction == FACTION_NEUTRAL)
				|| (rules->getAllowTargetHostileGround() && faction == FACTION_HOSTILE))
			: ((rules->getAllowTargetFriendStanding() && faction == FACTION_PLAYER) || (rules->getAllowTargetNeutralStanding() && faction == FACTION_NEUTRAL)
				|| (rules->getAllowTargetHostileStanding() && faction == FACTION_HOSTILE));
	};
	auto treatableStanding = [&](const BattleUnit *patient) {
		return rules->getAllowTargetStanding() && (patient->isWoundable() || rules->getAllowTargetImmune()) && sideAllowed(patient, false);
	};
	auto treatableLying = [&](const BattleUnit *patient) {
		return !patient->isBigUnit() && rules->getAllowTargetGround() && (patient->isWoundable() || rules->getAllowTargetImmune()) && sideAllowed(patient, true);
	};
	// The unconscious unit lying on a tile (the first), if any.
	auto bodyOn = [&](Tile *tile) -> BattleUnit* {
		for (BattleItem *item : tile ? *tile->getInventory() : std::vector<BattleItem*>())
		{
			if (item->getUnit() && item->getUnit() != unit && item->getUnit()->getStatus() == STATUS_UNCONSCIOUS)
			{
				return item->getUnit();
			}
		}
		return nullptr;
	};

	// A unit standing under the crosshair: next to the soldier (the way to it needing the fewest
	// eighths of a turn), or out of range.
	BattleUnit *pointed = pointedUnit();
	if (pointed && pointed != unit)
	{
		int bestEighths = 9;
		for (int direction = 0; direction < 8; ++direction)
		{
			Position reachedTile;
			const int eighths = std::min((direction - unit->getDirection() + 8) % 8, (unit->getDirection() - direction + 8) % 8);
			if (eighths < bestEighths && tileEngine->validMeleeRange(unit->getPosition(), direction, unit, pointed, &reachedTile, false))
			{
				bestEighths = eighths;
				aim.direction = direction;
			}
		}
		if (aim.direction < 0)
		{
			aim.refusal = "STR_OUT_OF_RANGE";
		}
		else if (treatableStanding(pointed))
		{
			aim.patient = pointed;
		}
		else
		{
			aim.direction = -1;
			aim.refusal = "STR_THERE_IS_NO_ONE_THERE";
		}
		return aim;
	}

	// A body under the crosshair: at the feet, or elsewhere (stand over it); or looking down at
	// the feet, a body lying there.
	BattleUnit *bodyAtFeet = bodyOn(_save->getTile(unit->getPosition()));
	BattleUnit *pointedBody = _view->getBodyAtPointer();
	if (pointedBody && pointedBody != bodyAtFeet && pointedBody->getPosition() != unit->getPosition())
	{
		aim.refusal = "STR_FPS_MEDIKIT_STAND_OVER";
		return aim;
	}
	if (bodyAtFeet && treatableLying(bodyAtFeet) && (pointedBody || _view->getPitchDegrees() < -25.0))
	{
		aim.patient = bodyAtFeet;
		return aim;
	}

	// Nothing of these: the soldier, or else the game's own order.
	if (rules->getAllowTargetSelf())
	{
		aim.patient = unit;
		return aim;
	}
	if (bodyAtFeet && treatableLying(bodyAtFeet))
	{
		aim.patient = bodyAtFeet;
		return aim;
	}
	Position facedTile;
	if (tileEngine->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &facedTile, false))
	{
		const Tile *tile = _save->getTile(facedTile);
		BattleUnit *faced = tile ? tile->getUnit() : nullptr;
		if (faced && treatableStanding(faced))
		{
			aim.patient = faced;
			return aim;
		}
	}
	aim.refusal = "STR_THERE_IS_NO_ONE_THERE";
	return aim;
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

	// A medi-kit, on the patient pointed at (see medikitPatient).
	const MedikitAim aim = medikitPatient(unit, item);
	BattleUnit *patient = aim.patient;
	if (!patient)
	{
		_state->warning(aim.refusal);
		return;
	}
	TileEngine *tileEngine = _save->getTileEngine();
	const BattleMediKitType type = rules->getMediKitType();
	if (type && !((type == BMT_HEAL && item->getHealQuantity() > 0) || (type == BMT_STIMULANT && item->getStimulantQuantity() > 0)
		|| (type == BMT_PAINKILLER && item->getPainKillerQuantity() > 0)))
	{
		_state->warning("STR_NO_USES_LEFT");
		return;
	}
	// A patient beside the soldier but not in the way they face: the soldier turns to them
	// first (by the rules or for free, see turnBeforeUse).
	if (!turnBeforeUse(unit, aim.direction, type ? action->Time : 0))
	{
		return;
	}
	if (!type)
	{
		_game->pushState(new MedikitState(patient, action, tileEngine));
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
 * @param dual Both guns fire at once (dual wield): wider (see DualWieldAccuracy).
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

	if (dual) spreadDegrees /= FirstPerson::DualWieldAccuracy; // a gun in each hand: less accurate
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
 * The game's hook for where an FPS-style shot leaves from (see FirstPerson::freeAimOrigin): a
 * straight shot by the soldier the 3D view is with leaves from the eye's place across the tile
 * (with a lean, the leaned eye's), at the height the game gave it (the gun's, within a voxel of
 * the eye's). The game's own muzzle point sits on the tile's edge towards the target, up to 7
 * voxels from the middle, while the eye stays near the middle: past a corner the crosshair
 * cleared, the game's line could meet the wall (a player's report: the laser went into the near
 * wall). So what the crosshair sees, the round reaches. Arcing shots keep the game's origin (the
 * hand), and so does a shot while the eye isn't on the soldier's tile (a step being walked).
 * @param state The battlescape screen (it owns the first-person mode).
 * @param action The shot being fired.
 * @param origin The game's origin, in voxels, to change.
 */
void FirstPersonMode::freeAimFrom(BattlescapeState *state, const BattleAction &action, Position &origin)
{
	if (!state || !state->_firstPerson || !state->_firstPerson->isActive() || !action.actor || !action.weapon
		|| action.weapon->getArcingShot(action.type))
	{
		return;
	}
	const FirstPersonMode *firstPerson = state->_firstPerson;
	if (action.actor != firstPerson->_trackedUnit || action.actor->getPosition() != firstPerson->_trackedTile)
	{
		return;
	}
	origin.x = (int)std::floor(firstPerson->_eyeX + firstPerson->_leanShift[0]);
	origin.y = (int)std::floor(firstPerson->_eyeY + firstPerson->_leanShift[1]);
}

/**
 * The game's hook for a shot knocked aside by a failed close-quarters check (see
 * FirstPerson::closeQuartersDeflected): passed on to the first-person mode if the 3D view is
 * showing.
 * @param state The battlescape screen (it owns the first-person mode).
 * @param shooter The unit whose shot was knocked aside.
 * @param target The tile the shot now goes at.
 */
void FirstPersonMode::deflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target)
{
	if (state && state->_firstPerson && state->_firstPerson->isActive())
	{
		state->_firstPerson->followDeflectedShot(shooter, target);
	}
}

/**
 * Turns the view to where a shot knocked aside by a failed close-quarters check goes (the enemy
 * beside the soldier shoves the gun): the game has already turned the soldier to face it, so
 * the view goes along, and the soldier isn't turned back to the old view afterwards. Knocked
 * to the side, the view turns to that tile (the pitch stays); at the feet or straight up, it
 * tips down or up (the facing stays). Only for the soldier we look through.
 * @param shooter The unit whose shot was knocked aside.
 * @param target The tile the shot now goes at: a neighbour, the soldier's own (the feet) or the
 *               one above it.
 */
void FirstPersonMode::followDeflectedShot(const BattleUnit *shooter, const Position &target)
{
	if (!shooter || shooter != _save->getSelectedUnit())
	{
		return;
	}
	_snapTurnDegrees = _snapPitchDegrees = 0.0; // no snap turn carries on against it
	const Position from = shooter->getPosition();
	if (target.x == from.x && target.y == from.y)
	{
		const double pitch = target.z > from.z ? DeflectedUpPitch : DeflectedDownPitch;
		_view->rotate(0.0, pitch - _view->getPitchDegrees());
		return;
	}
	// The yaw has 0 = east, 90 = south (map y grows southwards).
	const double yawDegrees = std::atan2((double)(target.y - from.y), (double)(target.x - from.x)) * 180.0 / Pi;
	_view->rotate(std::remainder(yawDegrees - _view->getYawDegrees(), 360.0), 0.0);
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
 * Do a shotgun's pellets fly beside its round, asked by the game as it fires one: with the setting
 * "Shotgun pellets in flight" (a player's report: two guns' rounds fly side by side, so why not a
 * shotgun's pellets?), while the 3D view shows, whoever fires. Otherwise the game's own way: the
 * whole blast lands the moment it's fired.
 * @param state The battlescape screen.
 * @return True if they fly.
 */
bool FirstPersonMode::pelletsInFlight(BattlescapeState *state)
{
	return Options::fpsPelletsInFlight && state && state->_firstPerson;
}

/**
 * Do a shotgun's pellets, and the two rounds of an akimbo volley, each land on their own, asked by
 * the game as it fires them: with the setting "Each pellet on its own" (on), in either view,
 * whoever fires (a player's report on OXCE's "shotgun void": the lead pellet decides the whole
 * blast, and every pellet is lost with it when it flies off the map). Otherwise the game's own way.
 * @param state The battlescape screen.
 * @return True if each lands on its own.
 */
bool FirstPersonMode::eachPellet(BattlescapeState *state)
{
	return Options::fpsEachPellet && state && state->_firstPerson;
}

/**
 * The scale a stat bar is drawn at, in pixels per point (a player's report: "say a unit has 300 hp
 * and takes 100 damage. The bar still appears full", the game drawing a pixel per point and
 * cutting the rest off at the bar's end), by the setting "Stat bars":
 * - the game's own: a pixel per point;
 * - scaled: a pixel per point while every maximum of the stat among the player's units fits the
 *   bar, else one smaller scale for all of them, so the longest fits and a longer maximum is still
 *   a longer bar, in either view (the owner: "not all bars are equal width if their absolute values
 *   are different", and in 3D too). On the map every vanilla soldier fits, and looks as before;
 *   the 3D view's bars are far shorter (52 and 40 pixels: morale's 100 never fitted), so there
 *   most stats shrink, all soldiers' alike;
 * - percentage: the bar full at the unit's own maximum, in both views.
 * The outline is drawn a pixel longer than the maximum, so the room is the width less one.
 * @param state The battlescape screen.
 * @param stat Which stat (FirstPerson::StatBar).
 * @param maximum The unit's maximum of it.
 * @param barWidth The bar's width, in pixels.
 * @return Pixels per point.
 */
double FirstPersonMode::statBarScale(BattlescapeState *state, int stat, int maximum, int barWidth)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || Options::fpsStatBars == 0 || maximum <= 0)
	{
		return 1.0;
	}
	const double room = barWidth - 1;
	if (Options::fpsStatBars == 2)
	{
		return room / maximum;
	}
	// The longest maximum of the stat among the player's units (morale's is always 100).
	auto statMaximum = [stat](const BattleUnit *unit)
	{
		const UnitStats *stats = unit->getBaseStats();
		switch (stat)
		{
		case FirstPerson::STAT_TU: return (int)stats->tu;
		case FirstPerson::STAT_ENERGY: return (int)stats->stamina;
		case FirstPerson::STAT_HEALTH: return (int)stats->health;
		case FirstPerson::STAT_MANA: return (int)stats->mana;
		default: return 100;
		}
	};
	int longest = maximum;
	for (const BattleUnit *unit : *mode->_save->getUnits())
	{
		if (unit->getFaction() == FACTION_PLAYER && unit->getStatus() != STATUS_DEAD)
		{
			longest = std::max(longest, statMaximum(unit));
		}
	}
	return std::min(1.0, room / longest);
}

/**
 * A shotgun's pellet landed (ProjectileFlyBState::pelletLands), in 3D or on the map (the owner:
 * "at least cosmetically all pellets should impact, play sound and show bulletholes"): its ammo's
 * hit sound where it struck (at most PelletSoundsAtOnce within PelletSoundWindow: a volley's
 * pellets land together, and the mixer's channels are few), and, while the 3D view shows, sparks,
 * chunks and a bullet hole there, as for the round (none on a unit: it bleeds or sparks its own way).
 * @param state The battlescape screen.
 * @param at Where it struck, in voxels.
 * @param from A point further back on its path, for its direction.
 * @param impact What it struck (a VoxelType; V_OUTOFBOUNDS: nothing in the map).
 * @param weapon The weapon that fired it.
 * @param ammo Its ammo.
 * @param heard Whether it's heard (not one whose hit the explosion state brings, with its own sound).
 */
void FirstPersonMode::pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || impact == V_OUTOFBOUNDS || !ammo)
	{
		return;
	}
	// Heard: a few to a volley.
	const Uint32 now = SDL_GetTicks();
	if (now - mode->_pelletSoundsFrom > PelletSoundWindow)
	{
		mode->_pelletSoundsFrom = now;
		mode->_pelletSounds = 0;
	}
	if (heard && mode->_pelletSounds < PelletSoundsAtOnce)
	{
		++mode->_pelletSounds;
		state->_battleGame->playSound(ammo->getRules()->getHitSound(), at.toTile());
	}
	// Marked in the 3D view, while it shows (as the round's mark: on the map its world may not even
	// be built yet, a battle loaded there).
	if (impact != V_UNIT && mode->isActive())
	{
		mode->_view->markPelletImpact(at, from, weapon, ammo);
	}
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
			const double modeFactor = _fireMode == BA_AUTOSHOT ? 1.0 : _fireMode == BA_SNAPSHOT ? 0.8 : 0.6;
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
 * A mod's own name for one of an item's actions (a player's report: X-Com Files' XM29 calls its
 * aimed shot "Launch Missile", and the crosshair said AIMED; "for clarity" the crosshair keeps
 * the mod's name). The names the game's action menu shows: a shot's or a blow's (confAimed,
 * confSnap, confAuto, confMelee: name), a medi-kit's, a psi-amp's or scanner's use, priming and
 * unpriming. Only one the mod set counts, not the game's own default (STR_AIMED_SHOT and the
 * like), which the crosshair's short names (AIMED, SNAP) stand for; in capitals, as those are.
 * @param rules The item.
 * @param mode The action.
 * @return The name in capitals ("LAUNCH MISSILE", "QUICKBURST"), or empty when it keeps the game's.
 */
std::string FirstPersonMode::renamedAction(const RuleItem *rules, BattleActionType mode) const
{
	// The mod's name for the action, against the game's default for it.
	auto setByMod = [](const std::string &name, const char *gameDefault) { return name != gameDefault ? name : std::string(); };
	std::string name;
	switch (mode)
	{
	case BA_AIMEDSHOT: name = setByMod(rules->getConfigAimed()->name, "STR_AIMED_SHOT"); break;
	case BA_SNAPSHOT: name = setByMod(rules->getConfigSnap()->name, "STR_SNAP_SHOT"); break;
	case BA_AUTOSHOT: name = setByMod(rules->getConfigAuto()->name, "STR_AUTO_SHOT"); break;
	case BA_HIT: name = rules->getConfigMelee()->name; break; // (none by default: the game says Hit or Stun)
	case BA_USE: name = rules->getBattleType() == BT_MEDIKIT ? setByMod(rules->getMedikitActionName(), "STR_USE_MEDI_KIT") : rules->getPsiAttackName(); break;
	case BA_PRIME: name = setByMod(rules->getPrimeActionName(), "STR_PRIME_GRENADE"); break;
	case BA_UNPRIME: name = rules->getUnprimeActionName(); break;
	default: break;
	}
	if (name.empty())
	{
		return name;
	}
	std::string text = _state->tr(name);
	Unicode::upperCase(text);
	return text;
}

/**
 * Updates the crosshair and the two lines under it, every frame: the mode's name (with the
 * burst size), then the hit chance at what's under the crosshair and the time unit cost.
 * The hit chance is worked out as the game's own isometric cursor does it (the game's firing
 * accuracy, then its range dropoff, a mod's penalty for a target out of the soldier's line of
 * sight, and its maximum range), so the two agree. The
 * crosshair is yellow when the soldier can do it, red when he can't (time units, ammo, range),
 * purple for psi, and grey when it's not his move. Under it the accuracy line (in FPS style
 * the spread, instead of odds).
 * @param unit The soldier being looked through (may be null).
 * @param playerTurn It's the player's turn (otherwise only a plain crosshair).
 */
void FirstPersonMode::updateCrosshair(BattleUnit *unit, bool playerTurn)
{
	// Shown only in the 3D view: the crosshair with its setting, the lines with the "Shot info"
	// setting in the player's turn (with or without the crosshair). The hit ticks below play
	// either way.
	const bool show = isActive();
	_crosshair->setVisible(show && Options::fpsCrosshair);
	if (!show) _accuracyLine->setVisible(false); // (else shown below, with a reach)
	_txtMode->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	_txtShot->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	_txtCosts->setVisible(show && Options::fpsCrosshairInfo && Options::fpsCrosshairCosts && playerTurn);
	if (!show)
	{
		return;
	}
	// The lines sit right of the crosshair's middle; without the crosshair they move down a
	// little, under where a pointed unit's name shows, so the middle of the view stays clear.
	const int centreX = _crosshair->getX() + CrosshairSize / 2, centreY = _crosshair->getY() + CrosshairSize / 2;
	const int linesTop = Options::fpsCrosshair ? centreY - 9 : centreY + 22;
	_txtMode->setX(centreX + 14);
	_txtMode->setY(linesTop);
	_txtShot->setX(centreX + 14);
	_txtShot->setY(linesTop + 10);
	_txtCosts->setX(centreX + 14);
	_txtCosts->setY(linesTop + 20);

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
	const BattleActionType mode = (playerTurn && weapon) ? _fireMode : BA_NONE;
	int shots = 1;
	Uint8 color = grey;
	std::string modeText, shotText, costText;
	bool dual = false, inTurn = false, volley = false, eachFull = false; // both guns fire (dual wield); an arcing pair, gun after gun, each at full accuracy and its own cost (see fire); a volley (setting "Akimbo volley")
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
		// mode, loaded) and there's time for both (the more expensive gun's shot and a tenth more;
		// an arcing pair each its own: see dualWieldCharge); otherwise the chosen gun fires alone,
		// at full accuracy, as fire() decides too.
		BattleActionType offMode = BA_NONE;
		int offTime = 0, offShots = 0;
		RuleItemUseCost offCost; // (the other gun's whole cost, for the line of other costs)
		if (BattleItem *otherWeapon = canFire ? dualPartner(unit, weapon, mode, offMode) : nullptr)
		{
			const bool arcingPair = firesInTurn(weapon, mode, otherWeapon, offMode);
			offCost = BattleActionCost(offMode, unit, otherWeapon);
			offTime = offCost.Time;
			if (!arcingPair) offTime = FirstPerson::dualWieldCharge(cost.Time, offTime);
			dual = doomModeOn() || cost.Time + offTime <= unit->getTimeUnits();
			inTurn = dual && arcingPair;
			volley = dual && !arcingPair && Options::fpsAkimboVolley;
			eachFull = dual && arcingPair;
			offShots = otherWeapon->getActionConf(offMode)->shots;
		}

		// The hit chance at what's under the crosshair, for shots and throws (not in FPS style:
		// no odds there, the accuracy line shows the spread). As the game's isometric cursor works it out
		// (Map.cpp): the soldier's firing accuracy with this weapon and mode, less the dropoff
		// per tile beyond the weapon's aim range or short of its minimum range (then shown
		// yellow); zero (red) beyond its maximum range.
		std::ostringstream shotInfo;
		int accuracy = -1;
		Uint8 accuracyColor = greenText;
		// (A straight shot has one on the open sky too, at the point of air the shot aims at, see aimAt.)
		if ((isShot(mode) || mode == BA_THROW) && (_aimHitsSomething || (isShot(mode) && !weapon->getArcingShot(mode))) && !freeAimOn(mode))
		{
			const BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(mode, unit, weapon);
			accuracy = BattleUnit::getFiringAccuracy(attack, _game->getMod());
			if (dual && !eachFull) accuracy = (int)(accuracy * FirstPerson::DualWieldAccuracy); // (before the range dropoff, as the shot does)
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

		// Under it, the action's other costs (a player: "an energy cost per action if applicable,
		// as well as any other costs (health, stun, morale, whatever)"; setting "Expanded
		// crosshair info"): energy, health, stun, morale and mana, those it has, "EN 6  MP 4"
		// (a gain, a mod's negative cost, with a plus). Both guns' with dual wield, as the game
		// charges them: energy like the time units (the other gun the rest of the pair's, or its
		// own in full for a pair firing in turn), the rest each gun's own. One the soldier can't
		// pay, by the game's own check (BattleActionCost::haveTU: health must stay above the cost,
		// and above the stun with it), goes red, and the action can't be done.
		if (Options::fpsCrosshairCosts)
		{
			const int energy = cost.Energy + (dual ? (eachFull ? offCost.Energy : FirstPerson::dualWieldCharge(cost.Energy, offCost.Energy)) : 0);
			const int health = cost.Health + (dual ? offCost.Health : 0), stun = cost.Stun + (dual ? offCost.Stun : 0);
			const int morale = cost.Morale + (dual ? offCost.Morale : 0), mana = cost.Mana + (dual ? offCost.Mana : 0);
			const struct { int amount; bool short_; const char *label; } costs[] = {
				{ energy, unit->getEnergy() < energy, "STR_FPS_COST_ENERGY" },
				{ health, health > 0 && unit->getHealth() <= health, "STR_FPS_COST_HEALTH" },
				{ stun, stun > 0 && unit->getHealth() - unit->getStunlevel() <= stun + health, "STR_FPS_COST_STUN" },
				{ morale, unit->getMorale() < morale, "STR_FPS_COST_MORALE" },
				{ mana, unit->getMana() < mana, "STR_FPS_COST_MANA" } };
			for (const auto &entry : costs)
			{
				if (entry.amount == 0) continue;
				if (!costText.empty()) costText += "  ";
				const std::string amount = std::string(_state->tr(entry.label)) + " " + (entry.amount < 0 ? "+" + std::to_string(-entry.amount) : std::to_string(entry.amount));
				costText += entry.short_ ? Unicode::TOK_COLOR_FLIP + amount + Unicode::TOK_COLOR_FLIP : amount;
				canFire = canFire && !entry.short_;
			}
		}

		// The mode's name. FPS style: auto says to hold the trigger; otherwise auto shows its number
		// of rounds (both guns' with dual wield).
		// "DUAL" (a volley: "VOLLEY"; an arcing pair: "IN TURN") goes in front with dual wield.
		static const struct { BattleActionType mode; const char *name; } modeNames[] = {
			{ BA_SNAPSHOT, "STR_FPS_MODE_SNAP" }, { BA_AIMEDSHOT, "STR_FPS_MODE_AIMED" }, { BA_AUTOSHOT, "STR_FPS_MODE_AUTO" },
			{ BA_THROW, "STR_FPS_MODE_THROW" }, { BA_LAUNCH, "STR_FPS_MODE_LAUNCH" }, { BA_HIT, "STR_FPS_MODE_MELEE" },
			{ BA_PANIC, "STR_FPS_MODE_PANIC" }, { BA_MINDCONTROL, "STR_FPS_MODE_MIND_CONTROL" }, { BA_USE, "STR_FPS_MODE_USE" },
			{ BA_PRIME, "STR_FPS_MODE_PRIME" }, { BA_UNPRIME, "STR_FPS_MODE_UNPRIME" } };
		for (const auto &entry : modeNames)
		{
			if (entry.mode == mode) modeText = _state->tr(entry.name);
		}
		// A mode the mod renamed reads as the game's action menu has it: "LAUNCH MISSILE".
		const std::string renamed = renamedAction(rules, mode);
		if (!renamed.empty())
		{
			modeText = renamed;
		}
		// A spray burst being marked: "SPRAY 1/2", the points marked of the weapon's number.
		const BattleAction *gameAction = _state->_battleGame->getCurrentAction();
		if (gameAction->sprayTargeting && gameAction->actor == unit && gameAction->weapon == weapon)
		{
			modeText = _state->tr("STR_FPS_MODE_SPRAY").arg((int)gameAction->waypoints.size()).arg(weapon->getRules()->getSprayWaypoints());
		}
		// Doom real-time grenades primed on throw: PRIME sets the fuse for every grenade, "FUSE 3s".
		if (mode == BA_PRIME && doomModeOn() && Options::fpsDoomPrimeOnThrow && timedGrenade(weapon))
		{
			modeText = _state->tr("STR_FPS_DOOM_FUSE").arg(std::clamp(Options::fpsDoomGrenadeFuse, 1, FirstPerson::DoomFuseMaxSeconds));
		}
		// A medi-kit names its patient: "USE: Fred", "USE: SELF" (see medikitPatient); with
		// none, it can't be used.
		if (mode == BA_USE && rules->getBattleType() == BT_MEDIKIT)
		{
			const BattleUnit *patient = medikitPatient(unit, weapon).patient;
			modeText += ": " + (patient == unit ? std::string(_state->tr("STR_FPS_MEDIKIT_SELF")) : patient ? unitLabel(patient) : std::string("-"));
			canFire = canFire && patient;
		}
		if (mode == BA_AUTOSHOT)
		{
			modeText += freeAimOn(mode) ? std::string(" ") + std::string(_state->tr("STR_FPS_HOLD")) : " x" + std::to_string(shots + (dual ? offShots : 0)); // (both guns' rounds)
		}
		if (dual)
		{
			modeText = std::string(_state->tr(inTurn ? "STR_FPS_IN_TURN" : volley ? "STR_FPS_VOLLEY" : "STR_FPS_DUAL")) + " " + modeText;
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
		_txtCosts->setColor(canAct() ? yellowText : greyText);
		_txtCosts->setSecondaryColor(canAct() ? redText : greyText);
	}
	_txtMode->setText(modeText);
	_txtShot->setText(shotText);
	if (_txtCosts->getText() != costText) _txtCosts->setText(costText);

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

	// Redraw the crosshair only when what it shows has changed: all of that packed into one
	// number (mode, rounds, colour, hitmarker) and compared with the last one.
	const Uint64 crosshairKey = (Uint64)mode | ((Uint64)shots << 8) | ((Uint64)color << 16) | ((Uint64)hitMark << 24);
	if (crosshairKey != _crosshairDrawnFor)
	{
		_crosshairDrawnFor = crosshairKey;
		drawCrosshair(mode, shots, color, shadow, hitMark, hitColor);
	}

	// The accuracy line (setting "Accuracy line") under the crosshair, green (grey while he
	// can't act): its half-width how far to either side a round can land, seen through the
	// view's lens. In FPS style the spread (it grows with the bloom of the last rounds); with
	// the game's own targeting, for a straight shot, how far a successful accuracy roll can send
	// the round from what's under the crosshair (see hitCloudReach).
	int reach = 0;
	if (unit && weapon && Options::fpsAccuracyLine && freeAimOn(mode))
	{
		const double focalPixels = (_view->getWidth() / 2.0) / std::tan(fieldOfViewNow() * Pi / 360.0);
		reach = std::clamp((int)std::lround(std::tan((spreadFor(unit, mode, dual && !eachFull) + _bloom) * Pi / 180.0) * focalPixels), AccuracyLineLeast, AccuracyLineReach);
	}
	else if (unit && weapon && Options::fpsAccuracyLine && isShot(mode) && !weapon->getArcingShot(mode))
	{
		reach = hitCloudReach(unit, weapon, mode);
	}
	_accuracyLine->setX(centreX - AccuracyLineReach - 1);
	_accuracyLine->setY(centreY + AccuracyLineDrop - 2);
	_accuracyLine->setVisible(Options::fpsCrosshair && reach > 0);
	const Uint8 lineColor = canAct() ? nearestColor(palette, 130, 230, 100) : grey;
	const Uint64 lineKey = (Uint64)reach | ((Uint64)lineColor << 16);
	if (reach > 0 && lineKey != _accuracyLineDrawnFor)
	{
		_accuracyLineDrawnFor = lineKey;
		drawAccuracyLine(reach, lineColor, shadow);
	}
}

/**
 * Draws the accuracy line (setting "Accuracy line") onto its surface: a level line through the
 * middle, as wide to either side as the reach, with a small notch up at each end; each pixel
 * with a shadow one down and to the right, so it reads on any background.
 * @param reach The half-width, in pixels (at most AccuracyLineReach).
 * @param color Palette colour of the line.
 * @param shadow Palette colour of its shadow.
 */
void FirstPersonMode::drawAccuracyLine(int reach, Uint8 color, Uint8 shadow)
{
	// The points, from the line's middle (the surface's row 2 is the line's, the notches rise
	// two rows above it); the shadows go under all of them before the colour goes on top.
	std::vector<std::pair<int, int>> points;
	for (int x = -reach; x <= reach; ++x) points.push_back({ x, 2 });
	for (int side : { -reach, reach }) { points.push_back({ side, 1 }); points.push_back({ side, 0 }); }
	const int middle = AccuracyLineReach + 1;
	_accuracyLine->clear();
	_accuracyLine->lock();
	for (const auto &point : points) _accuracyLine->setPixel(middle + point.first + 1, point.second + 1, shadow);
	for (const auto &point : points) _accuracyLine->setPixel(middle + point.first, point.second, color);
	_accuracyLine->unlock();
}

/**
 * The accuracy line's reach with the game's own targeting (setting "Accuracy line"), for a
 * straight shot at what's under the crosshair: how far to either side of the point aimed at a
 * successful accuracy roll can send the round, as the game rolls it (Projectile::applyAccuracy).
 * The roll's deviation grows with the distance from the muzzle, by the game's own measure of it
 * (more across the map's y than along its x, unless OXCE's "Uniform shooting spread" is on), and
 * the round goes up to half of it to either side on the map (and an eighth of it up or down,
 * which the line leaves out), then flies on along that line. A roll under the accuracy strays
 * this far at most, whatever the soldier's skill (the hit chance says how likely that is; a miss
 * strays much wider).
 * @param unit The soldier aiming.
 * @param weapon The gun.
 * @param mode The shot.
 * @return The reach across the view, in pixels (0 with nothing to measure).
 */
int FirstPersonMode::hitCloudReach(BattleUnit *unit, BattleItem *weapon, BattleActionType mode)
{
	// From the muzzle the shot leaves from (a lean's side too, as fire picks it) to the point
	// under the crosshair.
	BattleAction shot;
	shot.actor = unit;
	shot.weapon = weapon;
	shot.type = mode;
	shot.target = aimedTile(_aimVoxel);
	shot.relativeOrigin = Options::fpsLeanShots || leaningOut() ? leanSide(unit, shot.target) : BattleActionOrigin::CENTRE;
	const Position origin = _save->getTileEngine()->getOriginVoxel(shot, _save->getTile(unit->getPosition()));

	// The game's measure of the distance, and its largest deviation for a successful roll.
	const int xDist = std::abs(origin.x - _aimVoxel.x), yDist = std::abs(origin.y - _aimVoxel.y), zDist = std::abs(origin.z - _aimVoxel.z);
	int xyShift;
	if (Options::oxceUniformShootingSpread)
	{
		xyShift = xDist <= yDist ? xDist / 4 + yDist : xDist + yDist / 4;
		xyShift *= 0.839;
	}
	else
	{
		xyShift = xDist / 2 <= yDist ? xDist / 4 + yDist : (xDist + yDist) / 2;
	}
	const int zShift = xyShift <= zDist ? xyShift / 2 + zDist : xyShift + zDist / 2;
	const double deviation = std::max(1, zShift * HitRollDeviation / 200);
	const double across = deviation / 2.0;

	// How far that reaches across the view, seen from the eye: the view's right at the point
	// (level, square to the line of sight).
	double eye[3];
	_view->getEye(eye);
	const double sight[3] = { _aimVoxel.x + 0.5 - eye[0], _aimVoxel.y + 0.5 - eye[1], _aimVoxel.z + 0.5 - eye[2] };
	const double distance = std::sqrt(sight[0] * sight[0] + sight[1] * sight[1] + sight[2] * sight[2]);
	const double level = std::sqrt(sight[0] * sight[0] + sight[1] * sight[1]);
	if (distance < 1.0 || level < 1e-6)
	{
		return 0;
	}
	const double right[2] = { -sight[1] / level, sight[0] / level };
	// (the map's spread: a square of it on x and y, or a circle with uniform spread)
	const double acrossReach = Options::oxceUniformShootingSpread ? across : across * (std::abs(right[0]) + std::abs(right[1]));

	const double focalPixels = (_view->getWidth() / 2.0) / std::tan(fieldOfViewNow() * Pi / 360.0);
	return std::clamp((int)std::lround(acrossReach / distance * focalPixels), AccuracyLineLeast, AccuracyLineReach);
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
	// From the soldier the view looks through (in the aliens' turn the game's selected unit is
	// the alien acting, whose tile the throw's sound would give away).
	BattleUnit *unit = _trackedUnit;
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
 * Every frame in 3D: finds the unit the player points at, under the crosshair or the free cursor,
 * for the name under it, the middle click, a medi-kit's patient and a melee blow (a player's
 * report, GitHub issue #26: the name showed only while a pixel of the unit's picture was under
 * the crosshair, and small units that move oddly, X-Com Files' swarmids, made it blink; the
 * owner: the hitbox first): the unit whose hitbox the game's own line through the pointer meets,
 * as a shot would (see aimAt: units out of sight are passed through; a head poking up into the
 * tile above counts for its unit, Tile::getOverlappingUnit), which doesn't change with the
 * picture's animation; else the one whose picture is there or within a few pixels of it (see
 * FirstPersonView::getUnitAtPointer: a unit far off is only a few pixels across).
 * @param unit The soldier looked through, or null (none pointed at).
 */
void FirstPersonMode::updatePointedUnit(BattleUnit *unit)
{
	_pointedUnit = nullptr;
	if (!unit || !isActive())
	{
		return;
	}
	int pointerX, pointerY;
	pointer(pointerX, pointerY);
	Position voxel;
	int hitType = V_EMPTY;
	if (aimAt(pointerX, pointerY, unit, voxel, &hitType) && hitType == V_UNIT)
	{
		const Tile *tile = _save->getTile(voxel.toTile());
		BattleUnit *hit = tile ? tile->getOverlappingUnit(_save) : nullptr;
		if (hit && hit != unit && !hit->isOut())
		{
			_pointedUnit = hit;
		}
	}
	if (!_pointedUnit)
	{
		_pointedUnit = _view->getUnitAtPointer();
	}
}

/**
 * Every frame in 3D: names the unit the player points at (setting "Unit names": see
 * updatePointedUnit) just under the crosshair, or the cursor, in its side's colour (green ours, red the aliens', yellow the
 * civilians'), as the game's unit screen names it; and tells the view where the player points,
 * and how much text is under it there (the icons over a downed unit go below it).
 * @param unit The soldier looked through, or null (nothing named).
 */
void FirstPersonMode::updateUnitName(BattleUnit *unit)
{
	updatePointedUnit(unit);
	int pointerX, pointerY;
	pointer(pointerX, pointerY);
	const BattleUnit *target = Options::fpsUnitNames ? pointedUnit() : nullptr;
	_txtTarget->setVisible(target != nullptr);
	// The name a line lower while the action's other costs show under the shot line (its text
	// would run into them), and under the accuracy line while that shows (see updateCrosshair);
	// a downed unit's icons under both.
	const int lineRoom = accuracyLineShown() ? AccuracyLineRoom : 0;
	const int below = std::max(costLineShown() ? 10 : 0, lineRoom);
	_view->setPointer(pointerX, pointerY, target ? _txtTarget->getHeight() + 1 + below : lineRoom);
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
	_txtTarget->setY(_view->getY() + pointerY + 10 + below);
}

/**
 * Whether the line of the action's other costs shows under the crosshair (see updateCrosshair):
 * the unit name and the icons over a downed unit go a line lower then.
 * @return True if it's shown with something on it.
 */
bool FirstPersonMode::costLineShown() const
{
	return _txtCosts && _txtCosts->getVisible() && !_txtCosts->getText().empty() && Options::fpsCrosshair;
}

/**
 * Whether the accuracy line shows under the crosshair (see updateCrosshair): the unit name and
 * the icons over a downed unit go under it then.
 * @return True if it's shown.
 */
bool FirstPersonMode::accuracyLineShown() const
{
	return _accuracyLine && _accuracyLine->getVisible();
}

/**
 * The middle click as on the isometric map (setting "Middle click identifies"): on a unit the
 * player points at (one in sight, standing, its hitbox or picture under the crosshair or the
 * cursor: see updatePointedUnit), the game's own unit screen (who it is and what it
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
	BattleUnit *target = pointedUnit();
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
 * After a shot of ours, once the game is done with it (or refused it), stops the game aiming that
 * shot again. On the isometric map the game keeps aiming after a shot, so the player can fire
 * again; in 3D every click sets its own shot up, and the leftover aim would eat the next right
 * click (zoom), Q / E or a classic HUD hand or SPECIAL click, which cancel an aim first. The map's
 * cursor goes back to the normal one with it (a player's report: after a shot in 3D the map showed
 * the aim cursor while a click there walked or turned the soldier). Called in both views, so a
 * switch to the map while the shot still flies ends its aim too.
 */
void FirstPersonMode::endShotAim()
{
	BattlescapeGame *battleGame = _state->_battleGame;
	if (!_shotAimLeft || battleGame->isBusy())
	{
		return;
	}
	_shotAimLeft = false;
	_turnShotFired = nullptr;
	BattleAction *action = battleGame->getCurrentAction();
	if (action->targeting && isShot(action->type) && action->waypoints.empty())
	{
		action->targeting = false;
		action->type = BA_NONE;
		battleGame->setupCursor(); // (the normal cursor, as the action no longer aims)
	}
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
