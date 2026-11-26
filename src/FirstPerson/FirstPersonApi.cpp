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
 * FirstPersonApi.cpp: the bridge between the core engine and the add-on. Each function the core
 * calls (see FirstPersonApi.h) is passed on to FirstPersonMode, or registers the add-on's options
 * and its 3D VIEW options tab.
 */
#include "FirstPersonApi.h"
#include "FirstPersonGpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../Engine/Screen.h"
#include "../Engine/Surface.h"
#include <algorithm>
#include <map>
#include "FirstPersonMode.h"
#include "OptionsFirstPersonState.h"
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/OptionInfo.h"
#include "../Engine/Options.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Engine/Script.h"
#include "../Mod/Mod.h"
#include "../Engine/State.h"
#include "../Interface/Text.h"
#include "FirstPersonModeShared.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Tile.h"
#include "../Mod/MapData.h"
#include "../Battlescape/Pathfinding.h"
#include "../Engine/Language.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// The Meta Quest build (Android): a few settings start otherwise there (its CPU; the owner's choices).
#ifdef __ANDROID__
const bool QuestBuild = true;
#else
const bool QuestBuild = false;
#endif
/// The Quest build (its version code) whose changed defaults optionsLoaded sets once on a profile from before it.
const int QuestDefaultsVersion = 124;

/**
 * How many of the mod's 3D views there are: modes made (see create) and not yet destroyed; one
 * or more, a battle with the mod's view is on. For the rule fixes that come with the mod (see
 * keepsFacingOnOwnTile), which units ask without a way to the battle's screen.
 * (A count, not a switch. A save loaded during a battle makes the new battle's screen, and its
 * mode, before the old one closes: a switch set by the new one was cleared by the old one a frame
 * later, and for the rest of that battle the rule fixes were off: a shot at a wall the soldier
 * stood against turned him south again, the view with him: "Shoot this wall (spoiler, it spins
 * you around)", a player, GitHub issue #255.)
 * @return The count.
 */
int &viewsAlive()
{
	static int count = 0;
	return count;
}

/**
 * A key option of the add-on's, as the build's SDL has it: SDL 1.2's key code is a type of its
 * own (OptionInfo takes it as a key), SDL2's a plain number, which OptionInfo would take for a
 * number setting (the Controls list wouldn't show it, the Settings list would, as a number); the
 * SDL2 build's core has KeyOptionInfo for keys.
 * @param owner Whose option it is.
 * @param id Its name in options.cfg.
 * @param key The key it sets.
 * @param def Its default key.
 * @param desc Its description's string.
 * @param cat Its category's string.
 * @return The option.
 */
OptionInfo keyOption(OptionOwner owner, const std::string &id, SDL_Keycode *key, SDL_Keycode def, const std::string &desc, const std::string &cat)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return KeyOptionInfo(owner, id, key, def, desc, cat);
#else
	return OptionInfo(owner, id, key, def, desc, cat);
#endif
}

}

/**
 * Creates the first-person mode for a battlescape screen, but only when an active mod switches
 * the add-on on (`firstPerson: enabled: true`). Without such a mod the battlescape gets no mode
 * at all, and the calls below that take the mode do nothing, so the game plays exactly as
 * without the add-on.
 * @param game The game.
 * @param state The battlescape screen the mode belongs to.
 * @return The new mode, or nullptr when no active mod enables the add-on.
 */
FirstPersonMode *create(Game *game, BattlescapeState *state)
{
	if (!game->getMod()->getFirstPerson().isEnabled())
	{
		return nullptr;
	}
	++viewsAlive();
	return new FirstPersonMode(game, state);
}

/**
 * Destroys the first-person mode when its battlescape screen closes (nullptr is fine).
 * @param mode The mode, or nullptr.
 */
void destroy(FirstPersonMode *mode)
{
	if (mode)
	{
		viewsAlive() = std::max(0, viewsAlive() - 1);
	}
	delete mode;
}

/**
 * Does a unit keep its facing when it turns to a point on its own tile, straight above or below
 * it (BattleUnit::directionTo)? The game turned it south (atan2(0, -0)), so shooting at a wall it
 * stood against, or the floor at its feet, cost a turn first; in a battle with the mod's 3D view
 * it keeps its facing (and a turret its own). Without the mod, the game's own way.
 * @return True in a battle with the mod's 3D view.
 */
bool keepsFacingOnOwnTile()
{
	return viewsAlive() > 0;
}

/**
 * Does the game's auto-equip (soldiers with no saved loadout: a New Battle's, or never equipped)
 * hand out guns before melee weapons, the strongest first (setting "Auto-equip guns
 * first", a player's report, GitHub issue #46: soldiers in XPiratez and X-Com Files spawned with
 * claws, shovels and swords)? OXCE counts a two-handed melee weapon as a rifle, handed out first,
 * and offers the craft's kit in no fixed order. Off (the default), OXCE's own way, unchanged.
 * @param mod The game's rules (the setting only with the OpenXcom3D mod on).
 * @return True if guns come first.
 */
bool autoEquipGunsFirst(const Mod *mod)
{
	return Options::fpsAutoEquipGunsFirst && mod && mod->getFirstPerson().isEnabled();
}

/**
 * The main menu with the mod on: its version (the mod's own metadata, "1.14") in small letters
 * just right of the title's first line, "OpenXcom3D", its foot on the big letters' foot (a player,
 * GitHub issue #69: without the AppImage's name, no telling the version; the owner: "a smaller
 * space and our version number besides OpenXcom3D", then "Make the version no. Smaller"). A text
 * of its own (one text has one size a line), added to the menu like its others.
 * @param state The main menu.
 * @param title Its title (centred, big, its first line the mod's name).
 * @param mod The game's rules.
 */
void addMenuVersion(State *state, Text *title, const Mod *mod)
{
	if (!state || !title || !mod || !mod->getFirstPerson().isEnabled())
	{
		return;
	}
	const auto &mods = Options::getModInfos();
	const auto found = mods.find("OpenXcom3D");
	if (found == mods.end())
	{
		return;
	}
	Text *version = new Text(MenuVersionWidth, MenuVersionHeight);
	state->add(version, "text", "mainMenu");
	version->setSmall();
	version->setText(found->second.getVersion());
	// Just right of the name, centred in the title's width, their feet on one line.
	const int nameRight = title->getX() + (title->getWidth() + title->getTextWidth(0)) / 2;
	version->setX(nameRight + MenuVersionGap);
	version->setY(title->getY() + MenuVersionDrop);
}

/**
 * How a weapon ranks for the auto-equip's order (setting "Auto-equip guns first"): direct fire
 * before launchers, then the harder-hitting first. Its rounds are the ammo its shots (aimed, snap
 * or auto) fire, the ones for it in the craft's kit, or failing that any it takes, or the gun
 * itself when it needs no ammo. It's a launcher when its shots arc or every round of it is an
 * area blast (a damage type with a radius: rockets, mortar bombs, grenades); its power is one
 * round's, the strongest direct round's (or the strongest blast's, for a launcher): bursts and
 * pellets mostly miss at range, and counted in they put shotguns and miniguns before heavy plasma
 * and cannons. A melee weapon by its own power; anything else at 0.
 */
struct AutoEquipRank
{
	bool launcher = false; ///< arcing, or only blasts: handed out after the direct-fire guns
	double power = 0.0;    ///< the power of its strongest round
};

/**
 * Ranks a weapon for the auto-equip's order (see AutoEquipRank).
 * @param rule The item.
 * @param items The craft's kit (the ammo in it counts first).
 * @return Whether it's a launcher, and the power of its strongest round.
 */
static AutoEquipRank autoEquipRank(const RuleItem *rule, const std::vector<BattleItem*> &items)
{
	AutoEquipRank rank;
	if (rule->getBattleType() == BT_MELEE)
	{
		rank.power = rule->getPower();
		return rank;
	}
	if (rule->getBattleType() != BT_FIREARM)
	{
		return rank;
	}
	// The rounds its shots fire, by their ammo slots: those in the kit, else any it takes, else
	// the gun itself.
	std::vector<const RuleItem*> rounds;
	const std::pair<const RuleItemAction*, int> actions[3] = { { rule->getConfigAimed(), rule->getCostAimed().Time },
		{ rule->getConfigSnap(), rule->getCostSnap().Time }, { rule->getConfigAuto(), rule->getCostAuto().Time } };
	for (const auto &action : actions)
	{
		if (!action.first || action.second <= 0)
		{
			continue;
		}
		const std::vector<const RuleItem*> *ammo = rule->getCompatibleAmmoForSlot(action.first->ammoSlot);
		if (!ammo || ammo->empty())
		{
			rounds.push_back(rule);
			continue;
		}
		bool anyInKit = false;
		for (const RuleItem *round : *ammo)
		{
			const bool inKit = std::any_of(items.begin(), items.end(), [round](const BattleItem *item) { return item->getRules() == round; });
			if (inKit)
			{
				rounds.push_back(round);
				anyInKit = true;
			}
		}
		if (!anyInKit)
		{
			rounds.insert(rounds.end(), ammo->begin(), ammo->end());
		}
	}
	// Its strongest direct round, else its strongest blast (a launcher).
	double direct = -1.0, blast = -1.0;
	for (const RuleItem *round : rounds)
	{
		double &strongest = round->getDamageType()->isDirect() ? direct : blast;
		strongest = std::max(strongest, (double)round->getPower());
	}
	rank.launcher = rule->getArcingShot() || direct < 0.0;
	rank.power = std::max(0.0, direct >= 0.0 && !rule->getArcingShot() ? direct : std::max(direct, blast));
	return rank;
}

/**
 * The kit offered to the auto-equip in a fixed order (setting "Auto-equip guns first"): the
 * direct-fire guns before the launchers, the harder-hitting first (see autoEquipRank), then the
 * mod's own item order, then by type; so each pass hands out its best first (a player, GitHub
 * issue #76: the mod's order put X-Com Files' bows and old SMGs before its cannons and gauss
 * rifles; the owner: launchers after the direct-fire guns), the same in every battle (OXCE's own
 * order is where the items sit in memory, which changes between builds and runs).
 * @param items The craft's kit.
 */
void sortAutoEquip(std::vector<BattleItem*> &items)
{
	std::map<const RuleItem*, AutoEquipRank> ranks;
	for (const BattleItem *item : items)
	{
		ranks.emplace(item->getRules(), AutoEquipRank());
	}
	for (auto &entry : ranks)
	{
		entry.second = autoEquipRank(entry.first, items);
	}
	std::stable_sort(items.begin(), items.end(), [&ranks](const BattleItem *a, const BattleItem *b)
	{
		const RuleItem *ruleA = a->getRules(), *ruleB = b->getRules();
		const AutoEquipRank &rankA = ranks[ruleA], &rankB = ranks[ruleB];
		if (rankA.launcher != rankB.launcher)
		{
			return !rankA.launcher;
		}
		if (rankA.power != rankB.power)
		{
			return rankA.power > rankB.power;
		}
		if (ruleA->getListOrder() != ruleB->getListOrder())
		{
			return ruleA->getListOrder() < ruleB->getListOrder();
		}
		return ruleA->getType() < ruleB->getType();
	});
}

/**
 * Does this pass of the auto-equip take this item (setting "Auto-equip guns first")? The passes:
 * two-handed guns, one-handed guns, two-handed melee weapons, one-handed melee weapons, ammo, then
 * the rest (AutoEquipLeftoversPass; a flare only when it's dark enough), as OXCE's rifles,
 * pistols, ammo and leftovers but guns first. A gun is a firearm with an aimed, snap or auto shot
 * reaching past AutoEquipGunReach tiles: some mods' melee weapons are firearms whose "shot" is a
 * lunge of a tile or two (XPiratez: staffs and swords 1 to 3 tiles, whips and flamers up to 12,
 * bows from 13, guns 40 and more).
 * @param rule The item.
 * @param pass The pass, 0 to AutoEquipLeftoversPass.
 * @param takeFlares Whether it's dark enough for a flare.
 * @return True if the pass takes it.
 */
bool autoEquipTakes(const RuleItem *rule, int pass, bool takeFlares)
{
	const int AutoEquipGunReach = 12;
	const bool isGun = rule->getBattleType() == BT_FIREARM && rule->getMaxRange() > AutoEquipGunReach &&
		(rule->getCostAimed().Time > 0 || rule->getCostSnap().Time > 0 || rule->getCostAuto().Time > 0);
	switch (pass)
	{
	case 0: return rule->isRifle() && isGun;
	case 1: return rule->isPistol() && isGun;
	case 2: return rule->isRifle() && !isGun;
	case 3: return rule->isPistol() && !isGun;
	case 4: return rule->getBattleType() == BT_AMMO;
	case AutoEquipLeftoversPass: return !rule->isPistol() && !rule->isRifle() && (rule->getBattleType() != BT_FLARE || takeFlares);
	default: return false;
	}
}

/**
 * Gives the first-person mode its per-frame update (movement, camera, the 3D picture), if
 * there is a mode.
 * @param mode The mode, or nullptr.
 */
void think(FirstPersonMode *mode)
{
	if (mode)
	{
		mode->think();
	}
}

/**
 * Each frame before the battle's own think, after the player's input and the map's scrolling: the Live command feed's
 * drone noted where the player left it (its DYNAMIC off: the battle's own camera moves are taken back, see
 * FirstPersonMode::updateFeed), if there is a mode.
 * @param mode The mode, or nullptr.
 */
void beforeBattleThink(FirstPersonMode *mode)
{
	if (mode)
	{
		mode->beforeBattleThink();
	}
}

/**
 * Gives the first-person mode the first look at an input event (keys, mouse), before the
 * battlescape screen handles it.
 * @param mode The mode, or nullptr.
 * @param action The event.
 * @return True if the mode used the event up (the battlescape screen then ignores it).
 */
bool handle(FirstPersonMode *mode, Action *action)
{
	return mode && mode->handle(action);
}

/**
 * Lets the first-person mode draw the battlescape screen its own way (without the classic HUD
 * when that is hidden).
 * @param mode The mode, or nullptr.
 * @return True if the mode drew the screen; false to let the battlescape screen draw it normally.
 */
bool blit(FirstPersonMode *mode)
{
	return mode && mode->blit();
}

/**
 * Asks whether the mouse should be captured right now, so moving it turns the view (mouse look).
 * @param mode The mode, or nullptr.
 * @return True while the 3D view wants mouse look; always false without a mode.
 */
bool wantsMouseCapture(const FirstPersonMode *mode)
{
	return mode && mode->wantsMouseCapture();
}

/**
 * Tells the first-person mode that the battlescape screen was resized. The screen has already
 * moved all its surfaces (its HUD to the middle and the bottom), ours among them; the mode puts
 * its own back where they belong and remakes the 3D view at the new size.
 * @param mode The mode, or nullptr.
 * @param widthChange How much wider the screen got, in pixels (negative when narrower).
 * @param heightChange How much taller the screen got, in pixels (negative when shorter).
 */
void resized(FirstPersonMode *mode, int widthChange, int heightChange)
{
	if (mode)
	{
		mode->resized(widthChange, heightChange);
	}
}

/**
 * Settles settings that take their first value from an older one, once the options are loaded
 * (called when a battle or the 3D VIEW tab opens): the smoke (Original, Modern) from 0.9's
 * "See-through smoke": YES gives Modern, NO Original (1.0's Solid becomes Original too); the
 * akimbo (Off,
 * Full, Realistic) from 1.08's "Full akimbo" and "Realistic akimbo" (and with akimbo Off, those
 * two, which now tell whether it's on just now, off: a hand-edited profile can't have two guns
 * fire with the setting Off, the akimbo key refusing to switch them off); and 1.0's next fire
 * mode key, when the player had moved it off F (F is the flashlight now), once.
 */
void settleOptions()
{
	Options::fpsSmoke = smokeMode();
	Options::fpsAkimbo = akimboChoice();
	if (Options::fpsAkimbo == 0)
	{
		Options::fpsDualWield = Options::fpsAkimboRealistic = false;
	}
	if (Options::keyFpsNextMode != SDLK_f)
	{
		Options::keyFpsNextFireMode = Options::keyFpsNextMode;
		Options::keyFpsNextMode = SDLK_f;
	}
	// The second key for the isometric / 3D switch is V by default, which the auto shot had; the auto shot has no key
	// by default. So too on a profile from before (the owner: "The V key should be bound to 3d view by default and
	// auto key unbound"; such a profile kept the auto shot on V for a while, and had no second key for the switch).
	// Once (the option "fpsKeyDefaults" keeps that it was done): a profile with the auto shot on V has no key for the
	// auto shot, and V is the switch's second key where it has none. After that the two keys are the player's own.
	if (Options::fpsKeyDefaults < 1)
	{
		if (Options::keyFpsModeAuto == SDLK_v)
		{
			Options::keyFpsModeAuto = SDLK_UNKNOWN;
			if (Options::keyBattleFirstPerson2 == SDLK_UNKNOWN) Options::keyBattleFirstPerson2 = SDLK_v;
		}
		Options::fpsKeyDefaults = 1;
	}
	// And the tactical map's wheel goes up and down the map's levels (the owner: "for minimap the wheel should
	// rather change levels of map and not zoom"; Ctrl and the wheel zoom): the setting "Tactical map: wheel changes
	// level" is on by default, and a profile from before, written with the old default, is given it once too.
	if (Options::fpsKeyDefaults < 2)
	{
		Options::fpsMapWheelLevels = true;
		Options::fpsKeyDefaults = 2;
	}
	// (both on one key, by the player's hand: it is the switch's, in the 3D view too)
	if (Options::keyBattleFirstPerson2 != SDLK_UNKNOWN && Options::keyBattleFirstPerson2 == Options::keyFpsModeAuto)
	{
		Options::keyFpsModeAuto = SDLK_UNKNOWN;
	}
	// The rows that set several settings since the owner's critical pass (OptionsFirstPersonState, foldedChoice) have no
	// choice for two mixes a profile may have from before, by hand: the isometric view's tactical map without the 3D
	// view's, and see-through corpses without see-through crops. Each is given the other too (the row's last choice),
	// so nothing the player had switched on goes. On a desktop "Sprite upscaling" is Off or Finest: Smooth and Smooth +
	// detail are Finest (the Meta Quest keeps its four).
	if (Options::fpsTacticalMapIso && !Options::fpsTacticalMap) Options::fpsTacticalMap = true;
	if (Options::fpsSeeThroughCorpses && !Options::fpsSeeThroughCrops) Options::fpsSeeThroughCrops = true;
	if (!QuestBuild && (Options::fpsSpriteUpscale == 1 || Options::fpsSpriteUpscale == 2)) Options::fpsSpriteUpscale = 3;
	fixHiddenSettings();
	settlePresets(); // (after the akimbo setting has its value: the gameplay presets set it)
}

namespace
{

/// How a setting is kept off the 3D VIEW tab (see HiddenSettings).
enum HiddenAs
{
	HIDDEN_FIXED,     // not listed, and always at its default: a profile with another value is given the default
	HIDDEN_FOLDED,    // not listed: another setting's row sets it with its own (OptionsFirstPersonState, foldedChoice)
	HIDDEN_ELSEWHERE, // not listed: one of the game's own, on its own tab or key, left as the player has it
	HIDDEN_DESKTOP,   // the Meta Quest build's alone: listed there; on a desktop not listed, and always at its default
};

/// The settings the 3D VIEW tab doesn't list (the owner's critical pass of 2026-11-24: "keep all the hidden options
/// in code easily accessible if we need them in the future"). Each is an option as before, with its code, its texts
/// and its line in the profile; only its row is gone. To bring one back, take its line out of this table. Run with
/// OPENXCOM3D_ALL_SETTINGS=1, the tab lists every one and leaves each as the profile has it (to try one out; the
/// test suite runs that way).
const struct { const char *id; HiddenAs as; } HiddenSettings[] = {
	{ "fpsEachPellet", HIDDEN_FIXED },          // a fix, no taste: each of a shotgun's pellets on its own, none lost when the first leaves the map
	{ "fpsPelletsInFlight", HIDDEN_FIXED },     // the pellets seen flying (no one wants them invisible)
	{ "fpsSmoothFire", HIDDEN_FIXED },          // the flames smooth (off: the dot-by-dot look from before 1.14)
	{ "fpsMeleeFloors", HIDDEN_FIXED },         // floors struck too, with "Melee hits walls and downed"
	{ "fpsFallenSoldierCamera", HIDDEN_FIXED }, // the camera of a soldier who falls, as the other cameras of the aliens' turn
	{ "fpsFacingIcons", HIDDEN_FIXED },         // the facing icons, shown only while their key (Left Alt) is held anyway
	{ "fpsMiddleClickIdentify", HIDDEN_FIXED }, // the middle click on a unit as on the map
	{ "fpsCraftIcons", HIDDEN_FIXED },          // a craft's own icon, only where the mod has one
	{ "fpsSafeLanding", HIDDEN_FIXED },         // the stop before a step into the sea or a fall to death
	{ "fpsLiftBeams", HIDDEN_FIXED },           // the lifts' beams, with "Particles" (which they need)
	{ "fpsEyeLower", HIDDEN_FIXED },            // the eye 3 voxels below the game's eye height
	{ "fpsGhostOpacity", HIDDEN_FIXED },        // the parts without shape at 50%
	{ "fpsMapWheelLevels", HIDDEN_FIXED },      // the tactical map's wheel goes up and down its levels (Ctrl and the wheel zoom)
	{ "fpsHitFlash", HIDDEN_FIXED },            // a unit hit flashes, with "Muzzle flashes" (which says so: flashes from gunfire, hits and blasts)
	{ "fpsForceArcing", HIDDEN_FIXED },         // an arcing shot the game would refuse fires anyway (the owner's call)
	{ "fpsRightClickDoors", HIDDEN_FIXED },     // a right click on a door opens it, as on the map (the owner's call)
	{ "fpsWallPaint", HIDDEN_FIXED },           // All rooms: every wall's face the map never shows in the paint of the side it faces (GitHub issue #264: "lets have All rooms always")
	{ "fpsTacticalMapIso", HIDDEN_FOLDED },     // into "Tactical map": Off, In 3D, Both views
	{ "fpsCrosshairCosts", HIDDEN_FOLDED },     // into "Shot info": Off, Basic, Costs, Damage, Full
	{ "fpsCrosshairDamage", HIDDEN_FOLDED },    // the same
	{ "fpsAlienTurnFacing", HIDDEN_FOLDED },    // into "Alien turn": Map, 3D, Facing
	{ "fpsSoftShadows", HIDDEN_FOLDED },        // into "Shadows": Off, On, and on the card Soft, Lamps, Soft+lamps
	{ "fpsAllLightShadows", HIDDEN_FOLDED },    // the same
	{ "fpsSeeThroughCorpses", HIDDEN_FOLDED },  // into "See-through": Off, Crops, All (the owner's call: one row for both)
	{ "fpsFlightCostOverride", HIDDEN_FOLDED }, // into "Flight cost per level": Rules, 0 to 24
	{ "fpsDamageOverkill", HIDDEN_FOLDED },     // into "Damage numbers": Off, On, Overkill (GitHub issue #262)
	{ "FPS", HIDDEN_ELSEWHERE },                // the game's frame rate limit (its own General tab)
	{ "fpsCounter", HIDDEN_ELSEWHERE },         // the game's frame counter (F7)
	{ "fpsVrMode", HIDDEN_DESKTOP },            // VR: the Meta Quest's (a desktop has no headset; VR mode Off)
	{ "fpsVrEyeSize", HIDDEN_DESKTOP },
	{ "fpsVrWeapon", HIDDEN_DESKTOP },
	{ "fpsVrTurning", HIDDEN_DESKTOP },
	{ "fpsVrLaser", HIDDEN_DESKTOP },
	{ "fpsVrSmoothing", HIDDEN_DESKTOP },
	{ "fpsVrFoveation", HIDDEN_DESKTOP },
	{ "fpsVrCardEyes", HIDDEN_DESKTOP },
};

/// Does the tab list every setting, hidden ones too (OPENXCOM3D_ALL_SETTINGS=1)?
bool allSettingsListed()
{
	static const bool listed = getenv("OPENXCOM3D_ALL_SETTINGS") && std::strcmp(getenv("OPENXCOM3D_ALL_SETTINGS"), "1") == 0;
	return listed;
}

/// How a setting is hidden (see HiddenSettings), if it is: true and its way, else false.
bool hiddenAs(const std::string &id, HiddenAs &as)
{
	for (const auto &hidden : HiddenSettings)
	{
		if (id == hidden.id && (hidden.as != HIDDEN_DESKTOP || !QuestBuild))
		{
			as = hidden.as;
			return true;
		}
	}
	return false;
}

}

/// Setting "Obliteration in UFO and TFTD": a unit's health must go below minus this share of its full health for it to
/// leave no corpse (see overKillShare).
const float ClassicOverKill = 0.75f;

/**
 * Does a mod set give any weapon overkill of its own: an item that hurts (a weapon, its ammunition, a grenade, a blade:
 * its power, or its melee power, above 0) whose damage type (or melee one) doesn't ignore it (the big mods' `damageAlter:
 * IgnoreOverKill: false`)? The original games' own damage types all ignore it. (Not every item: one that hurts no one
 * keeps a damage type nobody set, which ignores nothing.) Found once a mod.
 * @param mod The game's rules.
 * @return True if some item has overkill.
 */
static bool ClassicOverkillHere = false; // (the last mod a hit was asked of has no overkill of its own: the setting holds, see overKillShare)
static bool modHasOverkill(const Mod *mod)
{
	static const Mod *known = nullptr;
	static bool has = false;
	if (mod == known) { ClassicOverkillHere = !has; return has; }
	known = mod;
	has = false;
	for (const std::string &name : mod->getItemsList())
	{
		const RuleItem *item = mod->getItem(name);
		if (!item) continue;
		const BattleType battle = item->getBattleType();
		const bool hurts = battle == BT_FIREARM || battle == BT_AMMO || battle == BT_GRENADE || battle == BT_PROXIMITYGRENADE || battle == BT_MELEE;
		const RuleDamageType *damage = item->getDamageType(), *melee = item->getMeleeType();
		if ((hurts && item->getPower() > 0 && damage && !damage->IgnoreOverKill) || (item->getMeleePower() > 0 && melee && !melee->IgnoreOverKill))
		{
			Log(LOG_VERBOSE) << "OpenXcom3D: \"Obliteration in UFO and TFTD\" stays out: " << name << " has overkill of its own (the loaded mods set it)";
			has = true;
			break;
		}
	}
	ClassicOverkillHere = !has;
	return has;
}

/**
 * Setting "Obliteration in UFO and TFTD": how far below 0 a unit's health must go, as a share of its full health, for
 * it to leave no corpse (BattleUnit::getOverKillDamage): with the setting, where it holds (see overkillDamageType),
 * ClassicOverKill (the owner: "Make it 75%"; OXCE's 0.5 for every armour of the original games made a plasma squad
 * lose most corpses; XPiratez's armours have 0.75 to 1, The X-Com Files' 1.5); else the armour's own.
 * @param armorShare The unit's armour's share.
 * @return The share to go by.
 */
float overKillShare(float armorShare)
{
	return Options::fpsObliterationClassic && ClassicOverkillHere ? ClassicOverKill : armorShare;
}

/**
 * Is the tile below a hole the top of solid ground (Tile::hasNoFloor)? The game counts a tile whose floor is gone as
 * having one only where the tile below is raised to its top (terrain level -24); a solid block that fills its tile
 * (a big wall "block", solid in every layer of its shape, that no one walks into) but is drawn with terrain level 0 didn't count,
 * so a unit whose floor melted over the rock under XPiratez's sewers fell into the rock, stuck there for good, and one
 * that came after it fell in on top and knocked it out (GitHub issue #266). Such a block's top is ground now: the hole
 * is walked over at the block's top, as the map draws it. Only with OpenXcom3D's mod on.
 * @param mod The game's rules.
 * @param below The tile under the hole.
 * @return True if its object is solid ground to stand on.
 */
bool solidGroundBelow(const Mod *mod, const Tile *below)
{
	if (!mod || !below || !mod->getFirstPerson().isEnabled())
	{
		return false;
	}
	const MapData *object = below->getMapData(O_OBJECT);
	if (!object || object->getBigWall() != Pathfinding::BLOCK || object->getTUCost(MT_WALK) < Pathfinding::INVALID_MOVE_COST)
	{
		return false;
	}
	for (int layer = 0; layer < 12; ++layer)
	{
		if (object->getLoftID(layer) == 0) return false; // (solid from its foot to its top: every layer of its shape)
	}
	return true;
}

/**
 * Setting "Obliteration in UFO and TFTD": the damage type a hit goes by (see TileEngine::hitUnit). With the setting on,
 * where no loaded mod gives any weapon overkill of its own (the original games, and mods that leave it as they have it),
 * an explosion's, a laser's and plasma's damage types (TFTD's Gauss is the laser's, Sonic the plasma's) don't ignore
 * overkill, as XPiratez and The X-Com Files give theirs (their overkill is on 137 of XPiratez's 200 explosives, 52 of its
 * 69 plasma weapons, 19 of its 83 lasers; The X-Com Files': 54 of 94, 33 of 38, 40 of 50; seldom a bullet, a blade or
 * a stun): a unit they take below minus ClassicOverKill of its health (see overKillShare) dies leaving no corpse, as
 * the map burns it, and with "Obliteration" bursts in 3D.
 * @param mod The game's rules.
 * @param type The hit's damage type.
 * @param scratch Room for an altered copy.
 * @return The type to go by: the hit's own, or the copy with overkill.
 */
const RuleDamageType *overkillDamageType(const Mod *mod, const RuleDamageType *type, RuleDamageType &scratch)
{
	if (!Options::fpsObliterationClassic || !type || !mod) return type;
	if (modHasOverkill(mod) || !type->IgnoreOverKill) return type; // (also notes whether the setting holds here: see overKillShare)
	if (type->ResistType != DT_HE && type->ResistType != DT_LASER && type->ResistType != DT_PLASMA) return type;
	scratch = *type;
	scratch.IgnoreOverKill = false;
	return &scratch;
}

/**
 * Whether the 3D VIEW tab leaves a setting out (see HiddenSettings; none with OPENXCOM3D_ALL_SETTINGS=1).
 * @param id The setting's option.
 */
bool isHiddenSetting(const std::string &id)
{
	HiddenAs as;
	return !allSettingsListed() && hiddenAs(id, as);
}

/**
 * Gives each setting the tab leaves out at a fixed value its default (see HiddenSettings): a profile that had
 * another value plays as everyone does. Called with settleOptions (a battle or the 3D VIEW tab opening). Nothing
 * with OPENXCOM3D_ALL_SETTINGS=1.
 */
void fixHiddenSettings()
{
	if (allSettingsListed()) return;
	for (const OptionInfo &option : Options::getOptionInfo())
	{
		HiddenAs as;
		if (hiddenAs(option.id(), as) && (as == HIDDEN_FIXED || as == HIDDEN_DESKTOP)) option.reset();
	}
}

namespace
{

/// Which tables of presets these are (see gameplaySwitches, settlePresets): 1 since the owner's rulings on GitHub
/// issue #193; 2 since "See-through corpses" is off in every preset (the owner: "should be off by default everywhere").
constexpr int PresetTables = 2;

/// A preset's value for a setting it leaves as it is (the player's, or what another group of presets has made it).
constexpr int Kept = -9999;

/**
 * One of the settings a preset sets: its option (a switch or a number) and its value under
 * each of the group's presets, in their order (Kept: that preset leaves it as it is); where a
 * change of the tables (see PresetTables) changed them, their values before it (Kept: it wasn't
 * listed): in the first tables, and in the second; and whether it is only set when a preset is
 * chosen, and is no part of what makes the preset (a setting another group owns: see
 * gameplaySwitches, the pixel size).
 */
struct PresetSwitch
{
	const char *id;
	bool *flag;
	int *number;
	std::vector<int> values;
	std::vector<int> before = {};   // (in tables 0, where they weren't as in tables 1)
	bool onChoice = false;
	std::vector<int> inTables1 = {}; // (in tables 1, where they weren't as now)
};

/**
 * A setting's value under a preset in the tables a profile saw (see PresetTables).
 */
int presetValueIn(const PresetSwitch &presetSwitch, int preset, int tables)
{
	int value = presetSwitch.values[preset];
	if (tables < 2 && !presetSwitch.inTables1.empty()) value = presetSwitch.inTables1[preset];
	if (tables < 1 && !presetSwitch.before.empty()) value = presetSwitch.before[preset];
	return value;
}

/**
 * A group of presets: the option that says which is chosen (0 .. the presets' number - 1, and
 * that number itself for Custom), the option that keeps the player's own mix ("id=value;..."),
 * and the settings they set.
 */
struct Presets
{
	int *chosen;
	std::string *mix;
	int count;
	const std::vector<PresetSwitch> &(*switches)();
};

// Thanks mumble for your dedication and work.
/**
 * The settings the gameplay presets set (setting "Gameplay", the first page of the 3D VIEW tab;
 * after GitHub issue #168, each keeping what the one before it set unless it says otherwise),
 * a column each:
 *  - FAITHFUL: soldiers' 3D view on top of OXCE, a graphics layer over the game;
 *  - Extended+: additional features, the intended play for most, and the defaults (the map
 *    camera, names, every shot's line, hitboxes always);
 *  - TACTICUS: a fuller tactical picture, information dense, fewer frills (cameras as the eyes
 *    see, the accuracy line, the rangefinder);
 *  - XMbl: TACTICUS for the veteran (akimbo, lean shots, modern throwing, damage numbers, a
 *    clearer and farther view);
 *  - FullQoL: every feature on that leaves the game's rules of turning, moving and shooting
 *    alone (sound, music, UFO lights, jumping, leaning, the live scanner, the volley, instant
 *    chains);
 *  - FPS: a shooter's rules on top (free turning, free walking, FPS-style shooting, quick
 *    deaths).
 * Aiming at the exact point is on in every one but FAITHFUL; realistic hitboxes (a shot hits
 * what is drawn there) from XMbl on.
 * A setting no preset changes isn't listed: it is the player's alone (every shot's line is
 * shown in all of them, FAITHFUL too, and so are all the parts without shape; the helmet
 * cameras' artifacts are Light and night vision is the futuristic one: those are the defaults).
 * The compass's step costs are TACTICUS's and XMbl's; sidesteps facing the view are XMbl's
 * alone; fall damage is XMbl's and FPS's.
 * The owner's rulings on a player's list (GitHub issue #193): the downed are always marked
 * from Extended+ on (FAITHFUL: under the crosshair); XMbl leans with a tilt, jumps low
 * obstacles and takes fall damage; guns come first at auto-equip and every room's walls are
 * painted in all six. Those two are listed although no preset differs in them: choosing a
 * preset sets them, and they are part of what makes a named preset (see settlePresets). So is
 * "See-through corpses", off in all six since the owner's "should be off by default
 * everywhere" (it was on from Extended+: tables 2).
 * And, of the same list, the owner's "Yes make both": from XMbl on the light is smooth
 * between tiles (the first three leave "Smooth lighting" as the player has it), and XMbl,
 * when it is chosen, sets the pixel size to 1, full detail. The pixel size is "Graphics
 * detail"'s (see graphicsSwitches: 2 at Low on the CPU): it is set on the choice only and is
 * no part of what makes XMbl, so a player on Low, or with a size of their own, is on XMbl
 * all the same. The same way XMbl, when it is chosen, makes the flashlight the far light (a
 * player, GitHub issue #245: "make far light preference for XMBL"): a profile on XMbl keeps
 * the flashlight it has.
 * @return The settings.
 */
const std::vector<PresetSwitch> &gameplaySwitches()
{
	using namespace Options;
	const int plain = DefaultViewDistance;
	static const std::vector<PresetSwitch> switches = {
		//                                                  FAITHFUL  Ext+  TACT  XMbl  QoL   FPS
		{ "fpsFaithfulTargeting", &fpsFaithfulTargeting, nullptr, { 1,    1,    1,    1,    1,    0 } },
		{ "fpsTurnCost",          &fpsTurnCost,          nullptr, { 1,    1,    1,    1,    1,    0 } },
		{ "fpsMovement",          nullptr, &fpsMovement,          { 1,    1,    1,    1,    1,    0 } },
		{ "fpsVault",             &fpsVault,             nullptr, { 0,    0,    0,    1,    1,    1 }, { 0, 0, 0, 0, 1, 1 } },
		{ "fpsLean",              nullptr, &fpsLean,              { 0,    0,    0,    2,    1,    1 }, { 0, 0, 0, 0, 1, 1 } },
		{ "fpsLeanShots",         &fpsLeanShots,         nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsSpriteHitboxes",    &fpsSpriteHitboxes,    nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsAimExact",          &fpsAimExact,          nullptr, { 0,    1,    1,    1,    1,    1 } },
		{ "fpsQuickDeaths",       &fpsQuickDeaths,       nullptr, { 0,    0,    0,    0,    0,    1 } },
		{ "fpsMeleeTerrain",      &fpsMeleeTerrain,      nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsLiveScanner",       &fpsLiveScanner,       nullptr, { 0,    0,    0,    0,    1,    1 } },
		{ "fpsFallDamage",        &fpsFallDamage,        nullptr, { 0,    0,    0,    1,    0,    1 }, { 0, 0, 0, 0, 0, 1 } },
		{ "fpsAkimbo",            nullptr, &fpsAkimbo,            { 0,    0,    0,    1,    1,    1 } },
		{ "fpsThrowing",          nullptr, &fpsThrowing,          { 0,    0,    0,    1,    1,    1 } },
		{ "fpsCloseEyeShots",     &fpsCloseEyeShots,     nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsAkimboVolley",      &fpsAkimboVolley,      nullptr, { 0,    0,    0,    0,    1,    1 } },
		{ "fpsForceBlocked",      &fpsForceBlocked,      nullptr, { 0,    0,    0,    0,    1,    1 } },
		{ "fpsInstantChains",     &fpsInstantChains,     nullptr, { 0,    0,    0,    0,    1,    1 } },
		{ "fpsAutoEquipGunsFirst", &fpsAutoEquipGunsFirst, nullptr, { 1,  1,    1,    1,    1,    1 }, { 0, 0, 0, 0, 1, 1 } },
		{ "fpsSidestepFaceView",  &fpsSidestepFaceView,  nullptr, { 0,    0,    0,    1,    0,    0 } },
		{ "fpsMapCamera",         nullptr, &fpsMapCamera,         { 0,    1,    1,    1,    1,    1 } },
		{ "fpsCameraEyes",        &fpsCameraEyes,        nullptr, { 0,    0,    1,    1,    1,    1 } },
		{ "fpsAlienTurnFacing",   &fpsAlienTurnFacing,   nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsUnitNames",         &fpsUnitNames,         nullptr, { 0,    1,    1,    1,    1,    1 } },
		{ "fpsCompassCosts",      &fpsCompassCosts,      nullptr, { 0,    0,    1,    1,    0,    0 } },
		{ "fpsHitboxes",          nullptr, &fpsHitboxes,          { 3,    2,    1,    1,    1,    1 } },
		{ "fpsAccuracyLine",      &fpsAccuracyLine,      nullptr, { 0,    0,    1,    1,    1,    1 } },
		{ "fpsRangefinder",       nullptr, &fpsRangefinder,       { 0,    0,    1,    1,    1,    1 } },
		{ "fpsDamageNumbers",     &fpsDamageNumbers,     nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsCrosshairDamage",   &fpsCrosshairDamage,   nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsGlowingOutlines",   &fpsGlowingOutlines,   nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsDownedIcons",       nullptr, &fpsDownedIcons,       { 1,    2,    2,    2,    2,    2 }, { 1, 1, 1, 1, 2, 2 } },
		{ "fpsWallPaint",         nullptr, &fpsWallPaint,         { 2,    2,    2,    2,    2,    2 }, { 1, 1, 1, 1, 2, 2 } },
		{ "fpsSeeThroughCrops",   &fpsSeeThroughCrops,   nullptr, { 0,    1,    1,    1,    1,    1 } },
		{ "fpsSeeThroughCorpses", &fpsSeeThroughCorpses, nullptr, { 0,    0,    0,    0,    0,    0 }, {}, false, { 0, 1, 1, 1, 1, 1 } },
		{ "fpsUnseenTerrain",     nullptr, &fpsUnseenTerrain,     { 0,    0,    2,    2,    2,    2 } },
		{ "fpsSmoothLighting",    &fpsSmoothLighting,    nullptr, { Kept, Kept, Kept, 1,    1,    1 }, { Kept, Kept, Kept, Kept, Kept, Kept } },
		{ "fpsPixelSize",         nullptr, &fpsPixelSize,         { Kept, Kept, Kept, 1,    Kept, Kept }, { Kept, Kept, Kept, Kept, Kept, Kept }, true },
		{ "fpsFlashlightReach",   nullptr, &fpsFlashlightReach,   { Kept, Kept, Kept, 1,    Kept, Kept }, { Kept, Kept, Kept, Kept, Kept, Kept }, true },
		{ "fpsHaze",              nullptr, &fpsHaze,              { 0,    0,    0,    2,    2,    2 } },
		{ "fpsViewDistance",      nullptr, &fpsViewDistance,      { plain, plain, plain, 130, 130, 130 } },
		{ "fpsSmokeOpacity",      nullptr, &fpsSmokeOpacity,      { 80,   80,   80,   65,   65,   65 } },
		{ "fpsNightSmokeOpacity", nullptr, &fpsNightSmokeOpacity, { 45,   45,   45,   65,   65,   65 } },
		{ "fpsUnitSize",          nullptr, &fpsUnitSize,          { 100,  100,  100,  90,   90,   90 } },
		{ "fpsDiscreetEdge",      &fpsDiscreetEdge,      nullptr, { 0,    0,    0,    1,    1,    1 } },
		{ "fpsUfoLights",         &fpsUfoLights,         nullptr, { 0,    0,    0,    0,    1,    1 } },
		{ "fpsHitSounds",         &fpsHitSounds,         nullptr, { 0,    0,    1,    0,    1,    1 } },
		{ "fpsCombatMusic",       &fpsCombatMusic,       nullptr, { 0,    0,    0,    0,    1,    1 } },
	};
	return switches;
}

/**
 * The settings "Graphics detail" sets (the first page of the 3D VIEW tab): how fine the 3D
 * world is drawn and what the graphics card adds, at Low, Medium and High. What a level means
 * goes by the setting "Renderer", so there are two tables. A ray colours one pixel at every
 * level but the CPU's Low (blocks of two). On the CPU: Low has those blocks and no sun shadows,
 * Medium the HUD's resolution (the defaults), High the battlescape's. On the card: Low is the
 * CPU's Medium picture drawn fast, Medium the display's own resolution with "Render scale" on
 * Auto (as fine as the frame rate allows), High that with full colour, smooth light, soft
 * shadows, ambient occlusion and the lights' effects. Supersampling and Surface relief are Off
 * at every level.
 * @return The settings, for the renderer chosen now.
 */
const std::vector<PresetSwitch> &graphicsSwitches()
{
	using namespace Options;
	static const std::vector<PresetSwitch> onCpu = {
		//                                                       Low   Med   High
		{ "fpsViewResolution",  nullptr, &fpsViewResolution,   { 0,    0,    1 } },
		{ "fpsPixelSize",       nullptr, &fpsPixelSize,        { 2,    1,    1 } },
		{ "fpsRenderScale",     nullptr, &fpsRenderScale,      { 100,  100,  100 } },
		{ "fpsShadows",         &fpsShadows,         nullptr,  { 0,    1,    1 } },
		{ "fpsSupersampling",   nullptr, &fpsSupersampling,    { 0,    0,    0 } },
		{ "fpsSoftShadows",     &fpsSoftShadows,     nullptr,  { 0,    0,    0 } },
		{ "fpsAllLightShadows", &fpsAllLightShadows, nullptr,  { 0,    0,    0 } },
		{ "fpsAmbientOcclusion", &fpsAmbientOcclusion, nullptr, { 0,   0,    0 } },
		{ "fpsTrueColor",       &fpsTrueColor,       nullptr,  { 0,    0,    0 } },
		{ "fpsSmoothLight",     &fpsSmoothLight,     nullptr,  { 0,    0,    0 } },
		{ "fpsColoredLights",   &fpsColoredLights,   nullptr,  { 0,    0,    0 } },
		{ "fpsReflections",     &fpsReflections,     nullptr,  { 0,    0,    0 } },
		{ "fpsLightShafts",     &fpsLightShafts,     nullptr,  { 0,    0,    0 } },
		{ "fpsSurfaceRelief",   nullptr, &fpsSurfaceRelief,    { 0,    0,    0 } },
	};
	static const std::vector<PresetSwitch> onCard = {
		//                                                       Low   Med   High
		{ "fpsViewResolution",  nullptr, &fpsViewResolution,   { 0,    2,    2 } },
		{ "fpsPixelSize",       nullptr, &fpsPixelSize,        { 1,    1,    1 } },
		{ "fpsRenderScale",     nullptr, &fpsRenderScale,      { 100,  0,    0 } },
		{ "fpsShadows",         &fpsShadows,         nullptr,  { 1,    1,    1 } },
		{ "fpsSupersampling",   nullptr, &fpsSupersampling,    { 0,    0,    0 } },
		{ "fpsSoftShadows",     &fpsSoftShadows,     nullptr,  { 0,    0,    1 } },
		{ "fpsAllLightShadows", &fpsAllLightShadows, nullptr,  { 0,    0,    1 } },
		{ "fpsAmbientOcclusion", &fpsAmbientOcclusion, nullptr, { 0,   0,    1 } },
		{ "fpsTrueColor",       &fpsTrueColor,       nullptr,  { 0,    0,    1 } },
		{ "fpsSmoothLight",     &fpsSmoothLight,     nullptr,  { 0,    0,    1 } },
		{ "fpsColoredLights",   &fpsColoredLights,   nullptr,  { 0,    0,    1 } },
		{ "fpsReflections",     &fpsReflections,     nullptr,  { 0,    0,    1 } },
		{ "fpsLightShafts",     &fpsLightShafts,     nullptr,  { 0,    0,    1 } },
		{ "fpsSurfaceRelief",   nullptr, &fpsSurfaceRelief,    { 0,    0,    0 } },
	};
	return fpsRenderer == 1 ? onCard : onCpu;
}

/**
 * A group of presets.
 * @param group Which.
 * @return It.
 */
const Presets &presetsOf(PresetGroup group)
{
	static const Presets gameplay = { &Options::fpsPreset, &Options::fpsCustomPreset, 6, gameplaySwitches };
	static const Presets graphics = { &Options::fpsGraphicsDetail, &Options::fpsCustomGraphics, 3, graphicsSwitches };
	return group == PRESET_GRAPHICS ? graphics : gameplay;
}

/// A preset's setting as it is now (a switch as 0 or 1).
int switchValue(const PresetSwitch &presetSwitch)
{
	// (Akimbo not yet settled, the defaults just restored: Off, see akimboChoice.)
	if (presetSwitch.number == &Options::fpsAkimbo && *presetSwitch.number < 0) return 0;
	return presetSwitch.flag ? (*presetSwitch.flag ? 1 : 0) : *presetSwitch.number;
}

/// Sets a preset's setting (a switch: 0 or not).
void setSwitch(const PresetSwitch &presetSwitch, int value)
{
	if (presetSwitch.flag) *presetSwitch.flag = value != 0;
	else *presetSwitch.number = value;
}

/// A group's settings as they are now, as Custom keeps them: "fpsTurnCost=0;fpsMovement=0;...".
std::string currentMix(const Presets &presets)
{
	std::string mix;
	for (const PresetSwitch &presetSwitch : presets.switches())
	{
		mix += std::string(presetSwitch.id) + "=" + std::to_string(switchValue(presetSwitch)) + ";";
	}
	return mix;
}

/// The preset a group's settings make now: the first whose values they all have, else Custom (the presets' number).
int presetOfSwitches(const Presets &presets)
{
	for (int preset = 0; preset < presets.count; ++preset)
	{
		bool all = true;
		for (const PresetSwitch &presetSwitch : presets.switches())
		{
			if (presetSwitch.onChoice || presetSwitch.values[preset] == Kept) continue;
			all = all && switchValue(presetSwitch) == presetSwitch.values[preset];
		}
		if (all) return preset;
	}
	return presets.count;
}

}

/**
 * How many presets a group has by name; Custom is the number after the last.
 */
int presetCount(PresetGroup group)
{
	return presetsOf(group).count;
}

/**
 * Chooses a preset (settings "Gameplay" and "Graphics detail"): a named one sets its settings;
 * Custom sets them back as the player had them (the group's kept mix). Leaving Custom keeps
 * the mix first, so trying a preset and coming back loses nothing. Nothing but the group's
 * settings is touched (one of them may be the other group's too: that group's name follows).
 * @param group Which presets.
 * @param preset One of them (0 ..), or their number for Custom.
 */
void choosePreset(PresetGroup group, int preset)
{
	const Presets &presets = presetsOf(group);
	if (*presets.chosen == presets.count)
	{
		*presets.mix = currentMix(presets);
	}
	*presets.chosen = std::clamp(preset, 0, presets.count);
	for (const PresetSwitch &presetSwitch : presets.switches())
	{
		if (*presets.chosen < presets.count)
		{
			if (presetSwitch.values[*presets.chosen] != Kept) setSwitch(presetSwitch, presetSwitch.values[*presets.chosen]);
			continue;
		}
		// Custom: the kept value, where there is one ("id=value;"); not of a setting only set on a choice (another
		// group's own).
		if (presetSwitch.onChoice) continue;
		const std::string key = std::string(presetSwitch.id) + "=";
		const size_t at = presets.mix->find(key);
		if (at != std::string::npos && (at == 0 || (*presets.mix)[at - 1] == ';'))
		{
			setSwitch(presetSwitch, std::atoi(presets.mix->c_str() + at + key.size()));
		}
	}
	// A setting set here that the other group lists too (the pixel size, XMbl's on its choice and "Graphics
	// detail"'s own): that group says the preset its settings make now, or Custom, keeping them as its mix.
	const Presets &others = presetsOf(group == PRESET_GAMEPLAY ? PRESET_GRAPHICS : PRESET_GAMEPLAY);
	const int made = presetOfSwitches(others);
	if (made != *others.chosen)
	{
		*others.chosen = made;
		if (made == others.count) *others.mix = currentMix(others);
	}
}

/**
 * Whether Custom can be chosen in a group: only once the player has a mix of their own (with
 * none kept, Custom would show its name over the last preset's settings).
 */
bool presetHasCustom(PresetGroup group)
{
	const Presets &presets = presetsOf(group);
	return *presets.chosen == presets.count || !presets.mix->empty();
}

/**
 * Notes a setting changed by hand: each group that sets it becomes the preset its settings now
 * make (a named one if they are all as it has them), else Custom, keeping the mix.
 * @param id The setting.
 */
void presetSwitchChanged(const std::string &id)
{
	for (PresetGroup group : { PRESET_GAMEPLAY, PRESET_GRAPHICS })
	{
		const Presets &presets = presetsOf(group);
		bool member = false;
		for (const PresetSwitch &presetSwitch : presets.switches()) member = member || id == presetSwitch.id;
		if (!member) continue;
		*presets.chosen = presetOfSwitches(presets);
		if (*presets.chosen == presets.count)
		{
			*presets.mix = currentMix(presets);
		}
	}
}

/**
 * The setting "Renderer" changed: a named "Graphics detail" means other settings on the card
 * than on the CPU (see graphicsSwitches), so it is set again for the new renderer, and Low
 * stays Low. Custom is the player's own and stays as it is.
 */
void rendererChanged()
{
	if (Options::fpsGraphicsDetail < presetCount(PRESET_GRAPHICS))
	{
		choosePreset(PRESET_GRAPHICS, Options::fpsGraphicsDetail);
	}
}

/**
 * Called once the player's options are read (Options::init, before the game saves them at its
 * start): on the Meta Quest build, the defaults that changed in build 115 (the owner's choice:
 * "VR eye picture" 460x500, "Sprite upscaling" Smooth+) are set once on a profile from
 * before (a profile keeps every value it saved, the old defaults too); the unlisted
 * fpsQuestDefaults then says so, and the player's own choices after that stay. Elsewhere nothing.
 * Also on that build: the add-on's diagnostic switches, which a desktop takes from the environment
 * (OPENXCOM3D_GPU_STAGES, OPENXCOM3D_GPU_TIMING, OPENXCOM3D_PASS_TIME, OPENXCOM3D_GPU_EFFECTS...), from a file an Android
 * app can be given: the profile folder's environment.txt, a NAME=VALUE a line, only names starting
 * OPENXCOM3D_. No file: nothing set.
 */
void optionsLoaded()
{
#ifdef __ANDROID__
	if (FILE *file = std::fopen((Options::getMasterUserFolder() + "environment.txt").c_str(), "r"))
	{
		char line[256];
		while (std::fgets(line, sizeof(line), file))
		{
			line[std::strcspn(line, "\r\n")] = 0;
			char *equals = std::strchr(line, '=');
			if (equals && std::strncmp(line, "OPENXCOM3D_", 11) == 0)
			{
				*equals = 0;
				setenv(line, equals + 1, 1);
				Log(LOG_INFO) << "OpenXcom3D: environment.txt sets " << line << " to " << equals + 1;
			}
		}
		std::fclose(file);
	}
	if (Options::fpsQuestDefaults < QuestDefaultsVersion)
	{
		// The Quest's preset, set once when a build with a newer one first runs (the player changes what they like
		// after). Build 124's is for the headset's graphics chip drawing the world: the sharpest picture that should
		// still keep a decent frame rate there, by a Quest 3's log of build 121 (an eye of 460x500 took 6 to 27 ms with
		// the chip at its low level). The eye is 710x770 with the rays thinned toward its edge (about a third of them
		// cast, fewer than a plain 460x500 eye's), smoothed twice as big by the chip (1420x1540, near the headset's own
		// 1680x1760), kept on the card and in full colour; what costs most for least (several rays a pixel, soft and
		// extra shadows, ambient occlusion, light shafts, reflections) is off, and so are voxel units, whose bodies the
		// CPU draws (the eyes would come back from the card).
		Options::fpsRenderer = 1;          // GPU
		Options::fpsVrCardEyes = true;
		Options::fpsVrEyeSize = 2;         // 710x770
		Options::fpsVrFoveation = 1;       // Light
		Options::fpsVrSmoothing = true;
		Options::fpsTrueColor = true;
		Options::fpsSmoothLight = true;
		Options::fpsColoredLights = true;
		Options::fpsSupersampling = 0;
		Options::fpsSoftShadows = false;
		Options::fpsAllLightShadows = false;
		Options::fpsAmbientOcclusion = false;
		Options::fpsLightShafts = false;
		Options::fpsReflections = false;
		Options::fpsVoxelUnits = false;
		Options::fpsSpriteUpscale = 2;     // Smooth+
		Options::fpsQuestDefaults = QuestDefaultsVersion;
		Log(LOG_INFO) << "OpenXcom3D: the Quest build " << QuestDefaultsVersion << "'s preset, set once: Renderer GPU, VR eye picture 710x770 with VR foveation Light and VR smoothing, "
			"eyes kept on the card in True colour with Smooth light and Coloured lights; Supersampling, Soft sun shadows, Shadows from every light, Ambient occlusion, "
			"Light shafts, Reflections and Voxel units off; sprite upscaling Smooth+";
	}
#endif
}

/**
 * Settles the presets with the settings as they are (a profile from before them, or one edited
 * by hand): each group says the preset its settings make, or Custom, keeping them as the mix.
 * Nothing but the two names changes: no setting of the player's is touched.
 * But once, for a profile from before the tables' last change (PresetTables; the option
 * "fpsPresetTables" keeps which tables a profile has seen): settings that are all as a named
 * preset had them in the tables the profile saw are that preset as it is now. Without it, every profile on
 * Extended+ would have turned Custom with the old values, and the new defaults would have
 * reached no one who had played before. A mix of the player's own matches no preset's old
 * values and is left alone.
 */
void settlePresets()
{
	if (Options::fpsPresetTables < PresetTables)
	{
		const Presets &gameplay = presetsOf(PRESET_GAMEPLAY);
		for (int preset = 0; preset < gameplay.count; ++preset)
		{
			bool all = true, changes = false;
			for (const PresetSwitch &presetSwitch : gameplay.switches())
			{
				// (a setting only set on a choice is no part of a preset; one not listed before, or left as it is, isn't asked)
				if (presetSwitch.onChoice) continue;
				const int now = presetSwitch.values[preset], was = presetValueIn(presetSwitch, preset, Options::fpsPresetTables);
				if (was != Kept) all = all && switchValue(presetSwitch) == was;
				changes = changes || (now != Kept && now != was);
			}
			if (!all) continue;
			if (changes)
			{
				for (const PresetSwitch &presetSwitch : gameplay.switches())
				{
					if (!presetSwitch.onChoice && presetSwitch.values[preset] != Kept) setSwitch(presetSwitch, presetSwitch.values[preset]);
				}
				Log(LOG_INFO) << "OpenXcom3D: the Gameplay preset " << preset << " as it is now (its settings were as it had them before)";
			}
			break;
		}
		Options::fpsPresetTables = PresetTables;
	}
	for (PresetGroup group : { PRESET_GAMEPLAY, PRESET_GRAPHICS })
	{
		const Presets &presets = presetsOf(group);
		const int made = presetOfSwitches(presets);
		if (made != *presets.chosen)
		{
			*presets.chosen = made;
			if (made == presets.count) *presets.mix = currentMix(presets);
		}
	}
}

/**
 * Sets the game's own options (OpenXcom's and OpenXcom Extended's, on the game's own tabs) to the
 * ones a player recommends for play with the add-on (GitHub issue #193: "things which, I think
 * outside the MOST purists, think are universally better to have on"): the row "Recommended
 * game options" on the first page of the 3D VIEW tab, when the player clicks it. No preset sets
 * them (the owner's ruling: a separate button): a preset is the add-on's own settings, and a
 * player's options of the game are theirs. An option a mod fixes for its players stays as the
 * mod has it. As any change on the options screens it is kept with OK and dropped with Cancel;
 * the scales take effect with OK.
 * The list: the Geoscape and the Battlescape at 640x400; on the globe the countries, the radars
 * and the flight paths, the funds shown, dragging with the middle button on the globe and on
 * the map; the path's arrows with their time units, tooltips; info screens maximized, values
 * changed by the wheel (by 10); the building queue, crafts launched whatever their state,
 * transfers of crafts in the air, the globe's realistic light; stats in the inventory,
 * grenades saved primed, explosions at height 2, battles ended by themselves, the alternate
 * movement methods, the line of fire overridden, the smooth bullet camera, no "Next Turn"
 * screen; OXCE's links, the clip sizes, a craft's maintenance time (2), the base's name in
 * popups, the base's large bars scaled, the accuracy by the crosshair (2).
 * @param mod The mod.
 * @return How many options it changed.
 */
int applyGameOptions(const Mod *mod)
{
	using namespace Options;
	struct GameOption { const char *id; bool *flag; int *number; int value; };
	const GameOption recommended[] = {
		{ "geoscapeScale", nullptr, &newGeoscapeScale, SCALE_2X },
		{ "battlescapeScale", nullptr, &newBattlescapeScale, SCALE_2X },
		{ "globeDetail", &globeDetail, nullptr, 1 },
		{ "globeRadarLines", &globeRadarLines, nullptr, 1 },
		{ "globeFlightPaths", &globeFlightPaths, nullptr, 1 },
		{ "showFundsOnGeoscape", &showFundsOnGeoscape, nullptr, 1 },
		{ "geoDragScrollButton", nullptr, &geoDragScrollButton, SDL_BUTTON_MIDDLE },
		{ "battleNewPreviewPath", nullptr, (int*)&battleNewPreviewPath, PATH_ARROW_TU },
		{ "battleTooltips", &battleTooltips, nullptr, 1 },
		{ "battleDragScrollButton", nullptr, &battleDragScrollButton, SDL_BUTTON_MIDDLE },
		{ "maximizeInfoScreens", &maximizeInfoScreens, nullptr, 1 },
		{ "changeValueByMouseWheel", nullptr, &changeValueByMouseWheel, 10 },
		{ "allowBuildingQueue", &allowBuildingQueue, nullptr, 1 },
		{ "craftLaunchAlways", &craftLaunchAlways, nullptr, 1 },
		{ "canTransferCraftsWhileAirborne", &canTransferCraftsWhileAirborne, nullptr, 1 },
		{ "globeSeasons", &globeSeasons, nullptr, 1 },
		{ "showMoreStatsInInventoryView", &showMoreStatsInInventoryView, nullptr, 1 },
		{ "includePrimeStateInSavedLayout", &includePrimeStateInSavedLayout, nullptr, 1 },
		{ "battleExplosionHeight", nullptr, &battleExplosionHeight, 2 },
		{ "battleAutoEnd", &battleAutoEnd, nullptr, 1 },
		{ "strafe", &strafe, nullptr, 1 },
		{ "forceFire", &forceFire, nullptr, 1 },
		{ "battleSmoothCamera", &battleSmoothCamera, nullptr, 1 },
		{ "skipNextTurnScreen", &skipNextTurnScreen, nullptr, 1 },
		{ "oxceLinks", &oxceLinks, nullptr, 1 },
		{ "oxcePediaShowClipSize", &oxcePediaShowClipSize, nullptr, 1 },
		{ "oxceInterceptGuiMaintenanceTime", nullptr, &oxceInterceptGuiMaintenanceTime, 2 },
		{ "oxceShowBaseNameInPopups", &oxceShowBaseNameInPopups, nullptr, 1 },
		{ "oxceBaseInfoScaleEnabled", &oxceBaseInfoScaleEnabled, nullptr, 1 },
		{ "oxceShowAccuracyOnCrosshair", nullptr, &oxceShowAccuracyOnCrosshair, 2 },
	};
	int changed = 0, fixed = 0;
	for (const GameOption &option : recommended)
	{
		if (mod && mod->getFixedUserOptions().count(option.id)) { ++fixed; continue; }
		if (option.flag)
		{
			if (*option.flag != (option.value != 0)) ++changed;
			*option.flag = option.value != 0;
		}
		else
		{
			if (*option.number != option.value) ++changed;
			*option.number = option.value;
		}
	}
	Log(LOG_INFO) << "OpenXcom3D: the recommended game options set, " << changed << " changed, " << fixed << " left as the mod fixes them";
	return changed;
}

/**
 * The akimbo setting as it stands: Off (0), Full (1, any two guns) or Realistic (2, one-handed
 * guns only). Unset (-1: a profile from 1.08 not settled yet, or the defaults just restored on the
 * 3D VIEW tab), it's what 1.08's two settings say, which now only tell whether akimbo is on just
 * now (the akimbo key switches them, see FirstPersonMode::toggleAkimbo): "Realistic akimbo" YES
 * Realistic, else "Full akimbo" YES Full, NO Off.
 * @return 0, 1 or 2.
 */
int akimboChoice()
{
	if (Options::fpsAkimbo >= 0 && Options::fpsAkimbo <= 2)
	{
		return Options::fpsAkimbo;
	}
	return Options::fpsAkimboRealistic ? 2 : (Options::fpsDualWield ? 1 : 0);
}

/**
 * The smoke setting as it stands: Original (1) or Modern (2). 1.0's Solid (0) is gone: it's
 * Original now. Unset (-1: a profile from 0.9 not settled yet, or the defaults just restored on
 * the 3D VIEW tab), it's what 0.9's "See-through smoke" says: YES Modern, NO Original.
 * @return 1 or 2.
 */
int smokeMode()
{
	if (Options::fpsSmoke == 1 || Options::fpsSmoke == 2)
	{
		return Options::fpsSmoke;
	}
	if (Options::fpsSmoke == 0)
	{
		return 1;
	}
	return Options::fpsSeeThroughSmoke ? 2 : 1;
}

/**
 * How far the big 3D view reaches (setting "View distance"): at its first value, 60, as far as
 * the mod has it (maxViewDistance: 60 in ours, so nothing changes; a mod that wants a shorter or
 * a longer view keeps it), else the distance chosen (10 to 200 tiles, as the mod's may be). The
 * small views (the helmet and map cameras) keep their own, shorter reach.
 * @param mod The game's rules.
 * @return The reach, in tiles.
 */
int viewDistance(const Mod *mod)
{
	return Options::fpsViewDistance == DefaultViewDistance ? mod->getFirstPerson().getMaxViewDistance()
		: std::clamp(Options::fpsViewDistance, 10, 200);
}

/**
 * Registers the add-on's options with the game's (called from Options::createOptionsOTHER, the
 * engine's place for a fork's own options), so they are loaded, saved and reset to their
 * defaults like any other option. Category STR_FIRST_PERSON is only listed by the 3D VIEW tab,
 * so they stay out of the other option screens; the tab lists them in the order they are
 * added here.
 * @param options The game's list of options, which they are added to.
 */
void createOptions(std::vector<OptionInfo> &options)
{
	using namespace Options;
	const char *category = "STR_FIRST_PERSON";

	// The settings (YES/NO switches and numbers), in the order the Settings list shows them.
	// First the headline feature: hovering a soldier's button shows that soldier's helmet camera.
	// The presets (see choosePreset): how it plays (FAITHFUL to FPS, Extended+ the default; Custom the number after the last)
	// and how fine it is drawn (Low, Medium, High; Custom), each with the player's own mix of its settings kept
	// (unlisted: no description).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPreset", &fpsPreset, 1, "STR_FPS_PRESET", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCustomPreset", &fpsCustomPreset, "", "", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPresetTables", &fpsPresetTables, 0, "", category)); // (which tables of presets the profile has seen: see settlePresets; unlisted)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsKeyDefaults", &fpsKeyDefaults, 0, "", category)); // (the defaults a profile from before was given, once each: see settleOptions; unlisted)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMapZoom", &fpsMapZoom, 0, "", category)); // (the tactical map's zoom as the player last had it; 0: not chosen yet; unlisted)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGraphicsDetail", &fpsGraphicsDetail, 1, "STR_FPS_GRAPHICS_DETAIL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCustomGraphics", &fpsCustomGraphics, "", "", category));
	// The game's own options set to a recommended lot, when the player clicks the row (see applyGameOptions): whether that was done.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGameOptions", &fpsGameOptions, false, "STR_FPS_GAME_OPTIONS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSoldierPreview", &fpsSoldierPreview, true, "STR_FPS_SOLDIER_PREVIEW", category));
	// And on the isometric map, the selected soldier's helmet camera in a window: off, small,
	// medium or large (0 to 3).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMapCamera", &fpsMapCamera, 1, "STR_FPS_MAP_CAMERA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTacticalMap", &fpsTacticalMap, true, "STR_FPS_TACTICAL_MAP", category));
	// The tactical map from the isometric battlescape too, in place of the game's own map screen (a player, GitHub
	// issue #217: "Tac map replace Normal map in iso? ... Still could make it toggle if you want for the purists").
	// Off by default: the isometric game stays the game's own unless asked.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTacticalMapIso", &fpsTacticalMapIso, false, "STR_FPS_TACTICAL_MAP_ISO", category));
	// On the tactical map the wheel changes the level and zooms with Ctrl (a player, GitHub issue #215: "mouse wheel
	// should default to elevation change, not zoom"). Off by default: the owner had the wheel zoom ("the mouse wheel
	// should not scroll levels of the map up down but zoom the map in/out").
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMapWheelLevels", &fpsMapWheelLevels, true, "STR_FPS_MAP_WHEEL_LEVELS", category));
	// The helmet cameras see as the soldier's eyes do (a player, GitHub issue #90: an X-Com Files
	// X-Bat's heat vision saw through smoke, its camera didn't): their heat vision through smoke
	// and fire. Off (the owner: a realistic camera, as before, by default): a camera hasn't the
	// wearer's eyes.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCameraEyes", &fpsCameraEyes, false, "STR_FPS_CAMERA_EYES", category));
	// The helmet cameras' artifacts (a player, GitHub issue #121: "Option to disable film grain in
	// mini camera ... LESS neat when Camera becomes illegible for longer than say, 250ms"): the
	// feeds' bad signal and spells of static as they are, Normal (0, the default); Light (1), a
	// lighter static and no glitch longer than LightGlitchLongest; or None (2), a clean feed.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCameraArtifacts", &fpsCameraArtifacts, 1, "STR_FPS_CAMERA_ARTIFACTS", category));
	// Mouse look: how fast the view turns (1 to 20), up and down swapped or not, and the field
	// of view across the screen, in degrees (50 to 110).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMouseSensitivity", &fpsMouseSensitivity, 5, "STR_FPS_MOUSE_SENSITIVITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsInvertMouse", &fpsInvertMouse, false, "STR_FPS_INVERT_MOUSE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFieldOfView", &fpsFieldOfView, 75, "STR_FPS_FIELD_OF_VIEW", category));
	// Zoom: holding the zoom key (or the right button in mouse look) looks 4 times closer.
	// The middle button as on the isometric map: a click on a unit in sight shows it (the game's
	// unit screen); anywhere else it frees the cursor, as always.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMiddleClickIdentify", &fpsMiddleClickIdentify, true, "STR_FPS_MIDDLE_CLICK_IDENTIFY", category));
	// Turning: free, the soldier facing wherever you look (NO), or by the game's rules (YES): the
	// head looks up to 45 degrees off the soldier's facing for free, further turns them at the
	// game's cost. YES by default (a player, GitHub issue #63: "It being off, is basically a cheat";
	// the defaults are the game's own mechanics).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTurnCost", &fpsTurnCost, true, "STR_FPS_TURN_COST", category));
	// Turning by the rules, a sidestep first turns the soldier to face the view (a step back: side-on), front armour to what is looked at.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSidestepFaceView", &fpsSidestepFaceView, false, "STR_FPS_SIDESTEP_FACE_VIEW", category));
	// The compass at the top right: where you look, the soldier's facing, and turning by the
	// rules, when looking further will cost time units.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCompass", &fpsCompass, true, "STR_FPS_COMPASS", category));
	// ...and round it, what a step each way costs in time units (and a level up or down, and a turn).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCompassCosts", &fpsCompassCosts, false, "STR_FPS_COMPASS_COSTS", category));
	// Moving: free walking or tile by tile (0 free, 1 tiles: a key press walks one tile), walking
	// speed in tiles per second (1 to 8; every tile still costs its time units),
	// the view bobbing with the steps, time units a flying soldier pays per level up or down
	// (0 = free) and whether that overrides the game's own cost for each unit, jumping over low
	// obstacles, falling off ledges, and how many voxels below the game's eye height the camera
	// sits (0 to 6).
	// (New installs start with the game's own mechanics where the 3D view adds its own: tile by
	// tile, flight at the game's cost, no vaulting; a player's report, GitHub issue #27.)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMovement", &fpsMovement, 1, "STR_FPS_MOVEMENT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsWalkSpeed", &fpsWalkSpeed, 4, "STR_FPS_WALK_SPEED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHeadBob", &fpsHeadBob, true, "STR_FPS_HEAD_BOB", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCost", &fpsFlightCost, 0, "STR_FPS_FLIGHT_COST", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCostOverride", &fpsFlightCostOverride, false, "STR_FPS_FLIGHT_COST_OVERRIDE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVault", &fpsVault, false, "STR_FPS_VAULT", category));
	// A right click on a closed door pointed at, within the soldier's reach, opens it, as on the isometric map (a
	// player, GitHub issue #244: "greater iso parity"); see FirstPersonMode::handleMouse.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsRightClickDoors", &fpsRightClickDoors, true, "STR_FPS_RIGHT_CLICK_DOORS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFalling", &fpsFalling, true, "STR_FPS_FALLING", category));
	// Fall damage, on its own (a player, GitHub issue #137: "Falling" off to play without the
	// damage also stopped every step off a ledge): off, a landing only jolts the view.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFallDamage", &fpsFallDamage, false, "STR_FPS_FALL_DAMAGE", category));
	// A step off a ledge onto ground no one can walk on, or where no step leads out (a player's
	// report: X-Com Files' blood moat, which the map never lets a soldier into, soft-locked one),
	// stops at the edge with a warning; pushing that way again (or jumping) goes over.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSafeLanding", &fpsSafeLanding, true, "STR_FPS_SAFE_LANDING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEyeLower", &fpsEyeLower, 3, "STR_FPS_EYE_LOWER", category));
	// Shooting: the game's own accuracy (YES) or FPS-style shooting along the crosshair (NO; shown
	// the other way round, as "FPS-style shooting (easier)", NO at first: a player's report, GitHub
	// issue #27, new players switched it thinking it the faithful way), and firing a gun in each
	// hand at once (akimbo): off (0), any two guns (1, full), or one-handed guns only (2,
	// realistic). Unset (-1) until settled (see settleOptions) from 1.08's two settings, which
	// stay, unlisted, as whether akimbo is on just now (the akimbo key switches them; at most one).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFaithfulTargeting", &fpsFaithfulTargeting, true, "STR_FPS_FPS_STYLE", category));
	// Recoil (a player, GitHub issue #67: for players whom a moving view bothers): off, the view
	// neither kicks up at each shot nor climbs in a burst; the gun in hand still kicks back.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsRecoil", &fpsRecoil, true, "STR_FPS_RECOIL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimbo", &fpsAkimbo, -1, "STR_FPS_AKIMBO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDualWield", &fpsDualWield, false, "STR_FPS_DUAL_WIELD", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimboRealistic", &fpsAkimboRealistic, false, "STR_FPS_AKIMBO_REALISTIC", "STR_FIRST_PERSON_SAVED"));
	// Akimbo's style: the two guns take turns round by round, each round resolved before the next
	// leaves, as an auto burst's (NO), or fire as a volley, both rounds at once and landing together
	// as a shotgun's pellets (YES); either way each less accurate, for the more expensive gun's shot
	// and a tenth. Its key switches it in play.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimboVolley", &fpsAkimboVolley, false, "STR_FPS_AKIMBO_VOLLEY", category));
	// The game's auto-equip hands out guns before melee weapons, in the mod's item order (a player's
	// report, GitHub issue #46; patch 113's way, now a choice). Off: OXCE's own auto-equip, unchanged.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAutoEquipGunsFirst", &fpsAutoEquipGunsFirst, true, "STR_FPS_AUTO_EQUIP_GUNS_FIRST", category));
	// Leaning: standing to one side of the tile's middle shoots from that side (the game's
	// off-centre origins), to peek round corners; and leaning out with the lean keys: off (0),
	// the eye moves to the side (1, peek), and the view tilts with it (2, tilt).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLeanShots", &fpsLeanShots, false, "STR_FPS_LEAN_SHOTS", category));
	// Throws and arcing shots: Classic (0), aimed at a tile's middle from one of the game's
	// eight points round the unit; or Modern (1), at the exact point under the crosshair from a
	// point that slides round the unit with the view, so the curve moves smoothly with the aim.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsThrowing", &fpsThrowing, 0, "STR_FPS_THROWING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLean", &fpsLean, 0, "STR_FPS_LEAN", category));
	// Melee at the wall, door or object the soldier faces (the game's terrain melee, on even if
	// the mod leaves it off) and at a body lying in front.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMeleeTerrain", &fpsMeleeTerrain, false, "STR_FPS_MELEE_TERRAIN", category));
	// With terrain melee on (that setting or the mod's): melee at the floor or ceiling under the
	// crosshair too.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMeleeFloors", &fpsMeleeFloors, true, "STR_FPS_MELEE_FLOORS", category));
	// Light, atmosphere and sound: smooth lighting between tiles, ray-traced shadows, a
	// flashlight, night vision only with the right gear, tenser music while aliens are in sight,
	// the ticks of a hit and a kill, smoke drawn as the soldier's eyes see through it, and
	// unlimited tactical flares.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothLighting", &fpsSmoothLighting, true, "STR_FPS_SMOOTH_LIGHTING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsShadows", &fpsShadows, true, "STR_FPS_SHADOWS", category));
	// The distance haze on land: normal (0), light (1), clear (2: only a thin fade where the
	// view ends). Under the sea the murk stays.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHaze", &fpsHaze, 0, "STR_FPS_HAZE", category));
	// Terrain the squad hasn't seen yet (the owner: three ways, from today's to the most costly, for
	// whole-map screenshots too): Mist (0), slate shapes turning misty further off, as since 1.03;
	// Fog (1), dark slate shapes (0.9's) in a fog rising from the edge of the game's sight to the
	// view's reach, as thick as the distance haze has it; Visible (2), drawn as seen terrain is, a
	// little greyer, in the usual haze. Units and items stay hidden until seen, whichever.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnseenTerrain", &fpsUnseenTerrain, 0, "STR_FPS_UNSEEN_TERRAIN", category));
	// How far the 3D view reaches, in tiles (see viewDistance): 60, as the mod has it, or further
	// (80, 100, 130, 160, 200), the haze closing in over the last stretch as before.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewDistance", &fpsViewDistance, DefaultViewDistance, "STR_FPS_VIEW_DISTANCE", category));
	// The fog of war in 3D (a player, GitHub issue #115: going down stairs, the floor below stayed
	// dark "despite looking directly at it" until the soldier stood on it): Off (0), terrain the view
	// itself sees within the game's sight range is drawn as seen, explored or not (beyond it the
	// setting "Unseen terrain" rules); As the map (1), only what the game has explored. The 3D view
	// only: the map and the game's exploring stay the game's, and units and items stay hidden until
	// seen, whichever.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFogOfWar", &fpsFogOfWar, 0, "STR_FPS_FOG_OF_WAR", category));
	// Wall paint (a player, GitHub issue #111: from behind a house its walls showed the beige of its
	// rooms, not its outside): the faces of walls the isometric art never shows, a west wall's west face
	// and a north wall's north face (see FirstPersonView::findWallPaints). As the art (0): the paint of
	// the face it shows, as before. Each side (1): a building's outer walls in the paint of the side
	// they face, its outside or the room inside. All rooms (2): the walls between its rooms too, each
	// face in its room's. The faces the map shows stay as it draws them; doors, and windows' frames and
	// glass, keep theirs. Since GitHub issue #264 no row on the tab and always All rooms (the owner: "lets have All rooms
	// always and thats it"; HiddenSettings, see WallPaintLevel in FirstPersonViewShared.h).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsWallPaint", &fpsWallPaint, 2, "STR_FPS_WALL_PAINT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCombatMusic", &fpsCombatMusic, false, "STR_FPS_COMBAT_MUSIC", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitSounds", &fpsHitSounds, false, "STR_FPS_HIT_SOUNDS", category));
	// The 3D view's ambient sounds (the owner: "The weather effects should also have nonobtrusive
	// sound. Namely rain and wind. The fire burning in front of you should also have a sound"),
	// made by the game (see FirstPersonAmbience): Off (0), Quiet (1, half as loud), On (2).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAmbientSounds", &fpsAmbientSounds, 2, "STR_FPS_AMBIENT_SOUNDS", category));
	// The smoke: the original games' own frames, see-through where the soldier sees through the
	// smoke and stacked where the sight runs out (1); soft clouds, solid where they block the
	// soldier's sight (2). (1.0's Solid, 0, is gone.) Unset (-1) until settled (see settleOptions)
	// from 0.9's "See-through smoke", kept below, unlisted.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoke", &fpsSmoke, -1, "STR_FPS_SMOKE", category));
	// The most opaque Modern's smoke is drawn, in percent: the thickest smoke's, thinner smoke less,
	// and smoke over smoke builds up to it and no further (a player's report, GitHub issue #98:
	// stacked smoke hid more than the game's sight rules); in the dark, by the light where it
	// hangs, the night value (a player: "non full smoke can be blinding" at night and on dark
	// maps), the two blended in between.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmokeOpacity", &fpsSmokeOpacity, 80, "STR_FPS_SMOKE_OPACITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightSmokeOpacity", &fpsNightSmokeOpacity, 45, "STR_FPS_NIGHT_SMOKE_OPACITY", category));
	// 0.9's "See-through smoke" (YES/NO), loaded and saved but listed nowhere (a category no
	// options screen shows): it gives the smoke setting its first value.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughSmoke", &fpsSeeThroughSmoke, true, "STR_FPS_SEE_THROUGH_SMOKE", "STR_FIRST_PERSON_SAVED"));
	// Looks: the weapon in hand (always made of voxels), sprite upscaling (0 off, 1 smooth,
	// 2 smooth + detail, 3 finest: six times bigger; the default, a profile saved before keeps its
	// value), muzzle flashes, particles (0 none, 1 effects, 2 everything),
	// UFOs with steady hulls, glowing lights, coloured light and an aura, and the pixel size
	// (1 to 3: one ray colours a block of pixels that wide and high, for speed).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewModel", &fpsViewModel, true, "STR_FPS_VIEW_MODEL", category));
	// (The Quest build starts at Smooth+, 2: the owner's choice for its CPU, build 115.)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteUpscale", &fpsSpriteUpscale, QuestBuild ? 2 : 3, "STR_FPS_SPRITE_UPSCALE", category));
	// Soldiers' and aliens' sprites a little smaller (80 to 100% of the isometric map's size).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitSize", &fpsUnitSize, 100, "STR_FPS_UNIT_SIZE", category));
	// Units (one tile big or two by two) as 3D bodies made from their own sprites (see VoxelUnits; the owner's sub-project).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVoxelUnits", &fpsVoxelUnits, false, "STR_FPS_VOXEL_UNITS", category));
	// The isometric map as a command post's screens (see CommandFeed; the owner: "Live command feed - herein the iso mode
	// is replaced with realistic combat dashboard"): the soldiers' helmet cameras in a row along the top, the map below as
	// a drone's feed, black and white with static, and the tactical map beside it. Experimental, off by default.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCommandFeed", &fpsCommandFeed, false, "STR_FPS_COMMAND_FEED", category));
	// The bodies twice as fine: a quarter of a voxel a cell, from the sprites enlarged twice (the owner: "Lets go with 2x").
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMuzzleFlash", &fpsMuzzleFlash, true, "STR_FPS_MUZZLE_FLASH", category));
	// The flashlight: 0 a beam from the weapon, a spot on the crosshair that fades with the distance; 1 a light where
	// the crosshair's line meets the world, the same at any range (a player's request, GitHub issue #232, after an
	// old shooter's flashlight: "it makes the flashlight usable at virtually any distance").
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlightReach", &fpsFlashlightReach, 0, "STR_FPS_FLASHLIGHT", category));
	// A unit hit, standing or down, flashes for a moment (the owner, on GitHub issue #74: its own
	// setting); off, it only flinches. "Muzzle flashes" off turns it off too (every flash: the
	// setting for players whom flashing light bothers).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitFlash", &fpsHitFlash, true, "STR_FPS_HIT_FLASH", category));
	// Fire's colours mixed from frame to frame (1.14's "Smoother fire", which had no setting: a
	// player looked for it, GitHub issue #146); off, the frames dissolve dot by dot, as before 1.14.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothFire", &fpsSmoothFire, true, "STR_FPS_SMOOTH_FIRE", category));
	// A unit killed beyond the game's own overkill (its rule for one that leaves no corpse) bursts into the pieces of its
	// body, and a blast bursts the bodies lying on the floor (a player, GitHub issue #257; the owner: "Lets just do
	// obliteration effect - also bodies on the floor"); off, it goes down and is gone, as on the map.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsObliteration", &fpsObliteration, true, "STR_FPS_OBLITERATION", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsBloodTrails", &fpsBloodTrails, false, "STR_FPS_BLOOD_TRAILS", category));
	// The original games' explosives, lasers and plasma (TFTD's Gauss and Sonic) with overkill, as the big mods give
	// theirs (the owner: "make a setting ... obliteration in UEU and TFTD on/off. And replicate xpiratez and xcom files
	// obliteration for og games also"): a unit they take past minus three quarters of its health (the owner: "Make it 75%") leaves no corpse (and with
	// "Obliteration" bursts in 3D). Only where no loaded mod gives any weapon overkill of its own. Off by default: it
	// changes the games' rules (corpses lost). See overkillDamageType.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsObliterationClassic", &fpsObliterationClassic, false, "STR_FPS_OBLITERATION_CLASSIC", category));
	// Screen shake (a player, GitHub issue #67): off, nothing shakes the view (explosions, hits,
	// falls, landings, panic, one's own shots).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsScreenShake", &fpsScreenShake, true, "STR_FPS_SCREEN_SHAKE", category));
	// Futuristic night vision (a player, GitHub issue #88: the map's night vision leaves units and
	// items their own colours): units, bodies and items keep theirs, the world goes green. Off (the
	// owner: realistic, by default), everything goes green.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionUnits", &fpsNightVisionUnits, true, "STR_FPS_NIGHT_VISION_UNITS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsParticles", &fpsParticles, 2, "STR_FPS_PARTICLES", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUfoLights", &fpsUfoLights, false, "STR_FPS_UFO_LIGHTS", category));
	// Parts the map draws but gives no shape (no voxels: ladders, lifts' pads, walls of laser
	// beams), shown as see-through ghosts of their art: 0 none, 1 lifts and ladders (the parts
	// with the grav lift flag), 2 all of them.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostParts", &fpsGhostParts, 2, "STR_FPS_GHOST_PARTS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostOpacity", &fpsGhostOpacity, 50, "STR_FPS_GHOST_OPACITY", category));
	// What is left of a sliding door once open, as a ghost: seen through, never in the way (see FirstPersonView::openDoorModel).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostOpenDoors", &fpsGhostOpenDoors, true, "STR_FPS_GHOST_OPEN_DOORS", category));
	// The crops, flower beds and rubble the game gives no real shape, drawn solid as on the map (see
	// buildRelief, buildCropBlock): a bit see-through instead (SeeThroughCropOpacity of the pixels),
	// so whoever stands in a field shows (the owner's ask, apart from the ghosts' opacity).
	// A lift anywhere (a building's elevator, an alien base's; a UFO's has its own with "UFO lights and aura"): a beam of light
	// rising through it, in the colours of its pad.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLiftBeams", &fpsLiftBeams, true, "STR_FPS_LIFT_BEAMS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughCrops", &fpsSeeThroughCrops, true, "STR_FPS_SEE_THROUGH_CROPS", category));
	// A corpse or item pointed at, the game's line passing it to something behind: drawn faint, at
	// the ghost opacity (the game's shots pass through them). Off by default and in every preset (the owner).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughCorpses", &fpsSeeThroughCorpses, false, "STR_FPS_SEE_THROUGH_CORPSES", category));
	// The map's edge as small red dots on the floor, nothing beyond it, instead of the tape.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDiscreetEdge", &fpsDiscreetEdge, false, "STR_FPS_DISCREET_EDGE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsRenderer", &fpsRenderer, 0, "STR_FPS_RENDERER", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewResolution", &fpsViewResolution, 0, "STR_FPS_VIEW_RESOLUTION", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHudScale", &fpsHudScale, 0, "STR_FPS_HUD_SCALE", category));
	// Fewer rays than pixels on the card, magnified (in percent each way; 0: Auto, by the frame rate): for a weak card.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsRenderScale", &fpsRenderScale, 100, "STR_FPS_RENDER_SCALE", category));
	// A finer picture than the old game's, on the graphics card only (Renderer: GPU), each off by default: several rays a
	// pixel, the sun's shadows with soft edges, a shadow from every lamp and flare, and corners and creases darkened.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSupersampling", &fpsSupersampling, 0, "STR_FPS_SUPERSAMPLING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSoftShadows", &fpsSoftShadows, false, "STR_FPS_SOFT_SHADOWS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAllLightShadows", &fpsAllLightShadows, false, "STR_FPS_ALL_LIGHT_SHADOWS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAmbientOcclusion", &fpsAmbientOcclusion, false, "STR_FPS_AMBIENT_OCCLUSION", category));
	// The 3D world in full colour, past the palette (the graphics card, 3D resolution Display, a 32-bit display), and the
	// light on it blended where the palette's picture dithers. Off by default.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTrueColor", &fpsTrueColor, false, "STR_FPS_TRUE_COLOR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothLight", &fpsSmoothLight, false, "STR_FPS_SMOOTH_LIGHT", category));
	// In full colour: shots in flight, explosions, fires and a muzzle flash light what is round them in their own colour.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsColoredLights", &fpsColoredLights, false, "STR_FPS_COLORED_LIGHTS", category));
	// In full colour: the sea mirrors the sky and what stands in it.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsReflections", &fpsReflections, false, "STR_FPS_REFLECTIONS", category));
	// With "Reflections": the alien entertainment panels' screens are still water standing on end, rippling and mirroring
	// the room (a player, GitHub issue #261: "trippy"; the owner: "Build it"). The mod's table names the panels
	// (`liquidScreens:`). On by default.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLiquidScreens", &fpsLiquidScreens, true, "STR_FPS_LIQUID_SCREENS", category));
	// In full colour: the flashlight, flares, lamps and coloured lights show as beams and glows in smoke and under the sea.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLightShafts", &fpsLightShafts, false, "STR_FPS_LIGHT_SHAFTS", category));
	// On the graphics card: walls, floors and hulls show a little relief read from their own drawings (0 off, 1 subtle, 2 strong). Off by default.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSurfaceRelief", &fpsSurfaceRelief, 0, "STR_FPS_SURFACE_RELIEF", category));
	// On the graphics card: a UFO's hull is metal (0 off; 1 sheen: it glints in the sun and under lamps; 2 mirror: and
	// faintly mirrors what is round it, with "Reflections"). Off by default.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMetallic", &fpsMetallic, 0, "STR_FPS_METALLIC", category));
	// VR with "Renderer": GPU, on a headset: the eyes' pictures stay on the graphics card and go to the headset's display
	// from there (nothing read back, nothing uploaded again). NO: read back and uploaded, as the CPU's eyes are.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrCardEyes", &fpsVrCardEyes, true, "STR_FPS_VR_CARD_EYES", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPixelSize", &fpsPixelSize, 1, "STR_FPS_PIXEL_SIZE", category));
	// The crosshair: shown or not, the shot info under it, aiming at the exact point under it
	// (YES) or at the tile, as the original game does (NO), and the units' hitboxes.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshair", &fpsCrosshair, true, "STR_FPS_CROSSHAIR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairInfo", &fpsCrosshairInfo, true, "STR_FPS_CROSSHAIR_INFO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairCosts", &fpsCrosshairCosts, true, "STR_FPS_CROSSHAIR_COSTS", category));
	// The shot's damage range under the hit chance, as the map shows it with Alt (a player, GitHub
	// issue #50; the owner: a setting, off).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairDamage", &fpsCrosshairDamage, false, "STR_FPS_CROSSHAIR_DAMAGE", category));
	// A rangefinder (a player, GitHub issue #139: "option to display tile distance from unit to
	// target"; the owner: "3 options: no, digital, analogue"): 0 off; 1 digital, left of the
	// crosshair the tiles to the point under it, as the game counts a shot's range; 2 analogue, a
	// scale in the manner of the PSO-1 scope's: stand a unit on its line, and where the head meets
	// the curve reads the tiles. The key below shows it while held, whatever the setting.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsRangefinder", &fpsRangefinder, 0, "STR_FPS_RANGEFINDER", category));
	// The soldier cards show each soldier's little figure from the craft screen, when the mod draws
	// one for the armor (a player, GitHub issue #53; the owner: the cards only, a setting, on).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCraftIcons", &fpsCraftIcons, true, "STR_FPS_CRAFT_ICONS", category));
	// A line under the crosshair as wide as a shot strays to either side: with the game's own
	// targeting, as far as a successful accuracy roll sends it at that range; in FPS style, the spread.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAccuracyLine", &fpsAccuracyLine, false, "STR_FPS_ACCURACY_LINE", category));
	// The name of the unit under the crosshair (or the free cursor), as the game names it.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitNames", &fpsUnitNames, true, "STR_FPS_UNIT_NAMES", category));
	// How the stat bars read, on the map and in 3D (a player's report: a 300-HP unit's bar looked
	// full with a third of its health gone): the game's own (0: a pixel per point, cut off at the
	// bar's end), scaled (1: a stat shrinks for all of the player's units alike when a maximum is
	// longer than the bar, on the map and in 3D), or percentage (2: each bar full at its maximum).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStatBars", &fpsStatBars, 1, "STR_FPS_STAT_BARS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAimExact", &fpsAimExact, true, "STR_FPS_AIM_EXACT", category));
	// With the game's own targeting, a straight shot at something within two tiles leaves from the
	// eye's place (the owner, on GitHub issue #148: "close range forced centre targeting"); off, from
	// the game's muzzle point as ever.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCloseEyeShots", &fpsCloseEyeShots, false, "STR_FPS_CLOSE_EYE_SHOTS", category));
	// The path a throw or shot would take, drawn while aiming: off (0), throws and arcing
	// shots (1), every shot too (2); and confirm fire: the first click marks the target and
	// shows its path, a second click on it fires.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTrajectory", &fpsTrajectory, 2, "STR_FPS_TRAJECTORY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsConfirmFire", &fpsConfirmFire, false, "STR_FPS_CONFIRM_FIRE", category));
	// An arcing shot the game would refuse (no curve of its own lands on the target tile, as at
	// a wall seen from the side) fires anyway, forced as with Ctrl held (its path drawn yellow).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsForceArcing", &fpsForceArcing, true, "STR_FPS_FORCE_ARCING", category));
	// A straight shot with the game's own targeting that finds no line of fire to the point under
	// the crosshair nor to any part of the target (off-centre too, with OXCE's option) fires anyway,
	// forced along the line to the crosshair, hitting what's in the way (its path drawn yellow).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsForceBlocked", &fpsForceBlocked, false, "STR_FPS_FORCE_BLOCKED", category));
	// A shotgun's pellets fly beside its round in the 3D view, each to where the game's spread
	// sends it, instead of all landing the moment it's fired (the game's own way, kept off).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPelletsInFlight", &fpsPelletsInFlight, true, "STR_FPS_PELLETS_IN_FLIGHT", category));
	// A shotgun's pellets, and the two rounds of an akimbo volley, each land on their own: none is
	// lost when the lead pellet leaves the map, and in flight each hits as it arrives (a player's
	// report on OXCE's "shotgun void"); off, the game's own way, the lead pellet deciding.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEachPellet", &fpsEachPellet, true, "STR_FPS_EACH_PELLET", category));
	// In 3D, a chain of terrain explosions set off by a blast settles at once, without its blasts to
	// watch (a player's request, GitHub issue #61: long chains are slow and heavy); off, the game's way.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsInstantChains", &fpsInstantChains, false, "STR_FPS_INSTANT_CHAINS", category));
	// The units' hitboxes, outlined: off (0), fading in and out once a second (1), always (2), or
	// only the unit pointed at (3, the first for new installs: a player's report, GitHub issue #27,
	// new players aimed at a Sectoid's picture and missed its hitbox).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitboxes", &fpsHitboxes, 2, "STR_FPS_HITBOXES", category));
	// A unit hit where its picture shows (in a line of fire, a card facing the shooter; see
	// FirstPersonSpriteHits.cpp), for everyone's shots, in 3D and on the map, instead of the game's
	// voxel shapes (the owner's, GitHub issue #42). Off: the game's own.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteHitboxes", &fpsSpriteHitboxes, false, "STR_FPS_SPRITE_HITBOXES", category));
	// A unit killed or knocked out down at once for the battle, its fall playing on its own (at the
	// game's pace, no turn to face the screen), several at once, in 3D and on the map; the soldier
	// looked through falls on their helmet camera card while the view goes on to the next (a
	// player's report, GitHub issue #40). Off: the game's own deaths, one by one.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsQuickDeaths", &fpsQuickDeaths, false, "STR_FPS_QUICK_DEATHS", category));
	// The motion scanner held up in 3D stays up as the soldier moves and through the aliens' turn,
	// a live radar for its one use (a player's report, GitHub issue #101: "potentially neat
	// feature"; the owner: a setting, off by default, in the playstyle). Off: one look from where
	// it was used, as the game's own scanner (see FirstPersonMode::updateScanner).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLiveScanner", &fpsLiveScanner, false, "STR_FPS_LIVE_SCANNER", category));
	// The hitboxes' outlines and the path of a throw glowing (spilling bloom round them), or plain lines.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGlowingOutlines", &fpsGlowingOutlines, false, "STR_FPS_GLOWING_OUTLINES", category));
	// Icons over the downed (unconscious) units in sight, for burning, bleeding, shock and
	// stunned: off (0), over the body under the crosshair (1), over every body in sight (2).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDownedIcons", &fpsDownedIcons, 2, "STR_FPS_DOWNED_ICONS", category));
	// With the key that shows which way the units face held: an arrow on each unit as well as the ring round its feet
	// (a player, GitHub issue #245: the ring "under foot is unreliable to see, depending on height and distance").
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFacingIcons", &fpsFacingIcons, true, "STR_FPS_FACING_ICONS", category));
	// The colours of the markers over the enemies (a player, GitHub issue #117: "markers should be red if visible, green
	// if unseen, and perhaps yellow if viable line of fire, but otherwise unseen"; the owner: a setting): all red (0, as
	// before), as the map's buttons for them (1: red if the soldier sees it, green if only the squad does), or that and
	// yellow for those of the green ones he has a clear line to (2, the default: Clear shot). None over our own: they
	// have the next setting.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEnemyMarkers", &fpsEnemyMarkers, 2, "STR_FPS_ENEMY_MARKERS", category));
	// Markers over our own that the map flags after the enemies (a player on GitHub issue #117: "wounded having their own
	// markers as blue, as iso, isn't a TERRIBLE idea"; the owner: a setting): off (0), or on (1, the default), each in its
	// button's colour and with its number, blue for the wounded, purple for those about to pass out.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsWoundedMarkers", &fpsWoundedMarkers, 1, "STR_FPS_WOUNDED_MARKERS", category));
	// A hit's damage rising over the unit hit and fading (the owner's): the health it took after its
	// armour in red, the armour it took off in blue, the stun in purple, a grey 0 when it did none of
	// these; every hit on a unit the squad sees, whoever struck, in 3D (in the world, behind walls
	// hidden) and on the map (see FirstPersonDamageNumbers.cpp). Off: none.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDamageNumbers", &fpsDamageNumbers, false, "STR_FPS_DAMAGE_NUMBERS", category));
	// And a killing blow's health past what the unit had, in orange after the red (a player, GitHub issue #262; the owner: "as
	// an expanded setting of damage numbers"): the row "Damage numbers" sets it, Off / On / Overkill (see foldedChoice).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDamageOverkill", &fpsDamageOverkill, false, "STR_FPS_DAMAGE_OVERKILL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitLogDetail", &fpsHitLogDetail, false, "STR_FPS_HIT_LOG_DETAIL", category)); // (the owner: whom, how much, who fell)
	// Staying in the 3D view during the alien turn, starting battles in it, and the debug cheat
	// "doom mode" (endless time units and energy), with its two extras, each doom mode plus
	// more: IDKFA (endless ammo) and IDDQD (invincible soldiers); in doom mode grenades go off in
	// real time (one click, a few seconds' fuse: the owner, "realtime grenade setting should be
	// always on with doom mode", where it was a setting of its own).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurn3D", &fpsAlienTurn3D, true, "STR_FPS_ALIEN_TURN_3D", category));
	// ...and in it, the view cuts to the soldier who sees the alien acting, or who fires back;
	// a soldier shot at, hit or taken over pops up their small camera.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurnCameras", &fpsAlienTurnCameras, true, "STR_FPS_ALIEN_TURN_CAMERAS", category));
	// ...and in the enemy's turn the view looks the way the soldier faces, at most the head's turn to either side.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurnFacing", &fpsAlienTurnFacing, false, "STR_FPS_ALIEN_TURN_FACING", category));
	// ...and with them, a soldier of ours going down out of view pops up their camera (static).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFallenSoldierCamera", &fpsFallenSoldierCamera, true, "STR_FPS_FALLEN_SOLDIER_CAMERA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStartInFirstPerson", &fpsStartInFirstPerson, false, "STR_FPS_START_IN_FIRST_PERSON", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomMode", &fpsDoomMode, false, "STR_FPS_DOOM_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIdkfa", &fpsDoomIdkfa, false, "STR_FPS_DOOM_IDKFA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIddqd", &fpsDoomIddqd, false, "STR_FPS_DOOM_IDDQD", category));
	// Doom mode plus noclip (a player's request, GitHub issue #45): in 3D the soldier walks and
	// floats through walls, objects, doors and floors, and X-COM's units don't fall.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIdclip", &fpsDoomIdclip, false, "STR_FPS_DOOM_IDCLIP", category));
	// VR mode (the Meta Quest 3 port; the owner: "These settings should be all under VR mode on/off!"): off (0, the
	// desktop's default: nothing changes anywhere), on (1: the VR rules, see FirstPersonModeVr.cpp: the head looks
	// anywhere, the soldier following past 45 degrees at the game's turning cost, tile moves as quick steps, free
	// walking slower, nothing moving the view but the head; the Quest build switches it on, its OpenXR shell drawing
	// the eyes), or on with the desktop's VR preview (2: both eyes side by side, the HUD over them at its depth, the
	// mouse as the head). Last in the list, so no row of the tab before it moves.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrMode", &fpsVrMode, 0, "STR_FPS_VR_MODE", category));
	// ...and each eye's picture: 360x390 (0), 460x500 (1) or 710x770 (2) pixels (the study's sizes for the Quest 3's
	// CPU: about 2.8, 3.6 and 5.6 pixels a degree). The Quest build starts at 460x500 (the owner's choice, build 115).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrEyeSize", &fpsVrEyeSize, QuestBuild ? 1 : 0, "STR_FPS_VR_EYE_SIZE", category));
#ifdef __ANDROID__
	// The Meta Quest build's defaults a profile has had (the build's version code; 0: none yet), unlisted: see
	// optionsLoaded.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsQuestDefaults", &fpsQuestDefaults, 0, "", "STR_FIRST_PERSON_SAVED"));
#endif
	// ...and the weapon in hand in VR (the owner: "should be a setting VR weapon on/off !!!"): YES, drawn in each eye as a
	// 3D object at its true depth, where the desktop's "Weapon in hand" puts it; NO, no weapon in VR at all (neither in
	// the eyes nor on the HUD). The desktop's own setting still rules the flat view.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrWeapon", &fpsVrWeapon, true, "STR_FPS_VR_WEAPON", category));
	// ...how the right stick turns the soldier (the owner: only the stick turns him, not the head nor the controller):
	// 0 Smooth (the default; VrSmoothTurnRate at full tilt), 1 Snap (45 degree steps); either way the world turns round
	// the player...
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrTurning", &fpsVrTurning, 0, "STR_FPS_VR_TURNING", category));
	// ...and the laser from the weapon's muzzle to where it points (YES), the crosshair at that point either way.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrLaser", &fpsVrLaser, true, "STR_FPS_VR_LASER", category));
	// ...and the eyes' pictures smoothed by the headset's graphics chip (the owner: "the blocky look is hard on the eyes"):
	// YES, each eye's picture drawn three times bigger with an edge-smoothing filter (FirstPersonXrSmoothing.cpp); NO,
	// the pixels as they are. Last in the VR section, so no row above it moves.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrSmoothing", &fpsVrSmoothing, true, "STR_FPS_VR_SMOOTHING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVrFoveation", &fpsVrFoveation, 0, "STR_FPS_VR_FOVEATION", category));
	// Doom mode's real-time grenades: armed as they're thrown, burning from leaving the hand (YES), or primed with a click and
	// burning in the hand (NO, "cooking");
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomPrimeOnThrow", &fpsDoomPrimeOnThrow, true, "STR_FPS_DOOM_PRIME_ON_THROW", category));
	// ...thrown freely at whatever the crosshair points at, bursting in the air when the fuse runs out in flight;
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomFreeGrenades", &fpsDoomFreeGrenades, false, "STR_FPS_DOOM_FREE_GRENADES", category));
	// ...and their fuse, in seconds (1 to 5; with prime on throw, FUSE in battle steps it too).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomGrenadeFuse", &fpsDoomGrenadeFuse, 3, "STR_FPS_DOOM_GRENADE_FUSE", category));

	// The add-on's own keys, in the order the Controls list shows them, with their default keys.
	// Switching views, freeing the cursor from mouse look, and showing the classic HUD.
	options.push_back(keyOption(OPTION_OTHER, "keyBattleFirstPerson", &keyBattleFirstPerson, SDLK_F8, "STR_FPS_KEY_TOGGLE_VIEW", category));
	// (A second key for it, under the left hand: GitHub issue #167. The auto shot, which had V, has no key of its own
	// by default now: the wheel and the next fire mode key reach it. On a profile from before too: see settleOptions.)
	options.push_back(keyOption(OPTION_OTHER, "keyBattleFirstPerson2", &keyBattleFirstPerson2, SDLK_v, "STR_FPS_KEY_TOGGLE_VIEW_2", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsMouseMode", &keyFpsMouseMode, SDLK_c, "STR_FPS_KEY_MOUSE_MODE", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsClassicHud", &keyFpsClassicHud, SDLK_h, "STR_FPS_KEY_CLASSIC_HUD", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsHideHud", &keyFpsHideHud, SDLK_p, "STR_FPS_KEY_HIDE_HUD", category));
	// Walking and strafing (W, A, S, D).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsForward", &keyFpsForward, SDLK_w, "STR_FPS_KEY_FORWARD", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsBack", &keyFpsBack, SDLK_s, "STR_FPS_KEY_BACK", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsStrafeLeft", &keyFpsStrafeLeft, SDLK_a, "STR_FPS_KEY_STRAFE_LEFT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsStrafeRight", &keyFpsStrafeRight, SDLK_d, "STR_FPS_KEY_STRAFE_RIGHT", category));
	// Turning and looking up and down with the keys (the arrows): the 3D view's own, not the map's
	// scroll keys it used before (a player, GitHub issue #81: with the map scrolled on W, A, S, D,
	// those keys walked and turned at once).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLookLeft", &keyFpsLookLeft, SDLK_LEFT, "STR_FPS_KEY_TURN_LEFT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLookRight", &keyFpsLookRight, SDLK_RIGHT, "STR_FPS_KEY_TURN_RIGHT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLookUp", &keyFpsLookUp, SDLK_UP, "STR_FPS_KEY_LOOK_UP", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLookDown", &keyFpsLookDown, SDLK_DOWN, "STR_FPS_KEY_LOOK_DOWN", category));
	// Two ways of walking, each on and off with its key: sprinting (the game's run; Caps Lock,
	// Doom's own "always run" key; it walks on past enemies coming into sight too) and en passant
	// (walking on past them, as Shift does on the map; the key left of 1, as Shift is the lean key).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsSprint", &keyFpsSprint, SDLK_CAPSLOCK, "STR_FPS_KEY_SPRINT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsEnPassant", &keyFpsEnPassant, SDLK_BACKQUOTE, "STR_FPS_KEY_EN_PASSANT", category));
	// Fire modes (the next one, or snap, aimed and auto directly) and switching hands. The next
	// fire mode is J (the game's "reserve TUs for kneel", which keeps it on the map), as F is the
	// flashlight. (1.0 saved it as keyFpsNextMode, on F: the new name leaves F behind, and a key
	// the player had picked instead carries over, see settleOptions.)
	options.push_back(keyOption(OPTION_OTHER, "keyFpsNextFireMode", &keyFpsNextFireMode, SDLK_j, "STR_FPS_KEY_NEXT_MODE", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsNextMode", &keyFpsNextMode, SDLK_f, "STR_FPS_KEY_NEXT_MODE", "STR_FIRST_PERSON_SAVED"));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsModeSnap", &keyFpsModeSnap, SDLK_z, "STR_FPS_KEY_MODE_SNAP", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsModeAimed", &keyFpsModeAimed, SDLK_x, "STR_FPS_KEY_MODE_AIMED", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsModeAuto", &keyFpsModeAuto, SDLK_UNKNOWN, "STR_FPS_KEY_MODE_AUTO", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsSwitchHand", &keyFpsSwitchHand, SDLK_g, "STR_FPS_KEY_SWITCH_HAND", category));
	// Akimbo on and off in battle (Y: one of the letters the battlescape leaves free).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsAkimbo", &keyFpsAkimbo, SDLK_y, "STR_FPS_KEY_AKIMBO", category));
	// Akimbo's style, both at once or in turn (setting "Akimbo in turn"): unbound unless the player binds one.
	options.push_back(keyOption(OPTION_OTHER, "keyFpsAkimboStyle", &keyFpsAkimboStyle, SDLK_UNKNOWN, "STR_FPS_KEY_AKIMBO_STYLE", category));
	// Turning 45 degrees at a time, without stepping (comma and period: "<" and ">").
	options.push_back(keyOption(OPTION_OTHER, "keyFpsTurnLeft", &keyFpsTurnLeft, SDLK_COMMA, "STR_FPS_KEY_SNAP_LEFT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsTurnRight", &keyFpsTurnRight, SDLK_PERIOD, "STR_FPS_KEY_SNAP_RIGHT", category));
	// Leaning out (setting "Lean"): the lean key held with a strafe key (Left Shift and A or D),
	// so Q and E stay the game's hand keys; and a key of its own for each side, held, unbound
	// unless the player binds one. (0.9's own lean keys, Q and E, were saved as keyFpsLeanLeft
	// and keyFpsLeanRight: the new names leave those behind, so Q and E are the hands again.)
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLeanHold", &keyFpsLeanHold, SDLK_LSHIFT, "STR_FPS_KEY_LEAN_HOLD", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLeanLeftOwn", &keyFpsLeanLeftOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_LEFT", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsLeanRightOwn", &keyFpsLeanRightOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_RIGHT", category));
	// Opening doors (O, as E is the game's "use right hand"), flares, flight, jumping, night
	// vision, going up and down a level, and doom mode.
	options.push_back(keyOption(OPTION_OTHER, "keyFpsOpenDoor", &keyFpsOpenDoor, SDLK_o, "STR_FPS_KEY_OPEN_DOOR", category));
	// Grabbing the item underfoot into a free hand, in 3D and on the map (GitHub issue #54): no key until
	// the player gives it one (every letter is the game's or ours).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsGrabItem", &keyFpsGrabItem, SDLK_UNKNOWN, "STR_FPS_KEY_GRAB_ITEM", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsFlare", &keyFpsFlare, SDLK_t, "STR_FPS_KEY_FLARE", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsFlight", &keyFpsFlight, SDLK_b, "STR_FPS_KEY_FLIGHT", category));
	// A tank's turret switch (a player's report: turning the view turned the whole tank, at its full
	// cost; the TURRET button on the top bar too): no key until the player gives it one.
	options.push_back(keyOption(OPTION_OTHER, "keyFpsTurret", &keyFpsTurret, SDLK_UNKNOWN, "STR_FPS_KEY_TURRET", category));
	// The flashlight on and off (F, as in the first-person shooters).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsFlashlight", &keyFpsFlashlight, SDLK_f, "STR_FPS_KEY_FLASHLIGHT", category));
	// Space is the map's night vision hold key; in 3D it jumps, and night vision is gear (N).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsJump", &keyFpsJump, SDLK_SPACE, "STR_FPS_KEY_JUMP", category));
	// The emergency climb (the owner: "a bindable key ... 50 TUs ... sometimes units can get stuck in level depressions
	// with no way out"): out of a pit, up a ledge, over a fence (see FirstPersonMode::emergencyClimb). No key at first.
	options.push_back(keyOption(OPTION_OTHER, "keyFpsEmergencyClimb", &keyFpsEmergencyClimb, SDLK_UNKNOWN, "STR_FPS_KEY_EMERGENCY_CLIMB", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsNightVision", &keyFpsNightVision, SDLK_n, "STR_FPS_KEY_NIGHT_VISION", category));
	// Zoom, held (U: free in the game and the add-on).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsZoom", &keyFpsZoom, SDLK_u, "STR_FPS_KEY_ZOOM", category));
	// The rangefinder on demand, held (setting "Rangefinder"; GitHub issue #139): no key until the
	// player gives it one (every letter is taken by the game or the add-on).
	options.push_back(keyOption(OPTION_OTHER, "keyFpsRange", &keyFpsRange, SDLK_UNKNOWN, "STR_FPS_KEY_RANGE", category));
	// Which way the units in sight face, held (Left Alt, as the map's own Alt shows its unit
	// indicators): a ring with an arrow on the floor round each, over everything.
	options.push_back(keyOption(OPTION_OTHER, "keyFpsShowFacing", &keyFpsShowFacing, SDLK_LALT, "STR_FPS_KEY_SHOW_FACING", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsUp", &keyFpsUp, SDLK_PAGEUP, "STR_FPS_KEY_UP", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsDown", &keyFpsDown, SDLK_PAGEDOWN, "STR_FPS_KEY_DOWN", category));
	options.push_back(keyOption(OPTION_OTHER, "keyFpsDoomMode", &keyFpsDoomMode, SDLK_INSERT, "STR_FPS_KEY_DOOM_MODE", category));
}

/**
 * Asks whether the options screen should offer the 3D VIEW tab: only when an active mod
 * switches the add-on on.
 * @param mod The game's loaded mods.
 * @return True to show the tab.
 */
bool hasOptionsTab(const Mod *mod)
{
	return mod->getFirstPerson().isEnabled();
}

/**
 * Asks whether a mod the game finds for the first time (one the player's mod list doesn't have
 * yet) starts switched on. Our own mod does, so a game built and laid out by hand plays in 3D
 * straight away, as a release (whose starting options list it on) does. The game asks only for
 * mods new to the list, so a player who switches it off keeps it off.
 * @param modId The new mod's id.
 * @return True to switch it on.
 */
bool activeWhenNew(const std::string &modId)
{
	return modId == "OpenXcom3D";
}

/**
 * Creates the 3D VIEW options tab, when its tab button is pressed.
 * @param origin The part of the game the options were opened from (main menu, geoscape or
 *  battlescape).
 * @return The new tab's screen.
 */
State *createOptionsTab(OptionsOrigin origin)
{
	return new OptionsFirstPersonState(origin);
}

/**
 * Called by the game as an FPS-style shot fires each round: replaces the round's target with
 * where the first-person view aims it now (along the crosshair, with the spread, and the
 * recoil climbing round by round). Nothing changes while the 3D view isn't showing.
 * @param state The battlescape screen, whose first-person mode aims the shot.
 * @param action The shot.
 * @param target The point the round flies to, in voxels; changed in place.
 */
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target)
{
	FirstPersonMode::freeAim(state, action, target);
}

/**
 * Called by the game as it works out where a shot leaves from (TileEngine::getOriginVoxel): an
 * FPS-style straight shot leaves from where the first-person view's eye is across the soldier's
 * tile, at the game's gun height (see FirstPersonMode::freeAimFrom). Nothing changes for any
 * other shot, or while the 3D view isn't showing.
 * @param state The battlescape screen.
 * @param action The shot.
 * @param origin The game's origin, in voxels; changed in place.
 */
void freeAimOrigin(BattlescapeState *state, const BattleAction &action, Position &origin)
{
	FirstPersonMode::freeAimFrom(state, action, origin);
}

/**
 * Called by the game as it works out where a shot leaves from (TileEngine::getOriginVoxel), after
 * it picked its own point: an arcing shot's or a throw's slides round the footprint instead (see
 * FirstPersonMode::slidingOrigin). Nothing changes for any other shot, with the setting off, or
 * while the 3D view isn't showing.
 * @param state The battlescape screen.
 * @param action The shot.
 * @param from The tile it leaves from (a large unit's: its first).
 * @param shiftX The game's eight points across a tile, by direction: along x, in voxels.
 * @param shiftY The same along y.
 * @param origin The game's origin, in voxels; changed in place.
 */
void slidingOrigin(BattlescapeState *state, const BattleAction &action, const Position &from, const int shiftX[8], const int shiftY[8], Position &origin)
{
	FirstPersonMode::slidingOrigin(state, action, from, shiftX, shiftY, origin);
}

/**
 * Called by the game when a close-quarters check knocks a shot aside: the 3D view looks where
 * the shot now goes (see FirstPersonMode::followDeflectedShot).
 * @param state The battlescape screen.
 * @param shooter The unit whose shot was knocked aside.
 * @param target The tile it now goes at.
 */
void closeQuartersDeflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target)
{
	FirstPersonMode::deflected(state, shooter, target);
}

/**
 * Called by the game as a bullet hits: should the next round follow without the usual wait?
 * It should for the player's FPS-style shots (faithful targeting off) while the 3D view shows,
 * so a burst plays quickly.
 * @param state The battlescape screen.
 * @param attacker The unit that fired.
 * @return True if the hits should play quicker.
 */
bool fastShots(BattlescapeState *state, const BattleUnit *attacker)
{
	return FirstPersonMode::fastShots(state, attacker);
}

/**
 * Called by the game as it fires a shotgun's round: do its pellets fly beside it (setting
 * "Shotgun pellets in flight", while the 3D view shows), rather than all landing at once?
 * @param state The battlescape screen.
 * @return True if the pellets should fly.
 */
bool pelletsInFlight(BattlescapeState *state)
{
	return FirstPersonMode::pelletsInFlight(state);
}

/**
 * Called by the game as a blast sets off a terrain explosion: does it settle at once, with the
 * rest of the chain (setting "Instant chain explosions", while the 3D view shows)?
 * @param state The battlescape screen.
 * @return True if the chain's links settle at once.
 */
bool instantChains(BattlescapeState *state)
{
	return FirstPersonMode::instantChains(state);
}

/**
 * Called by the game before it gives the battle its time slice: does it wait while a chain of
 * explosions settled at once ripples through in 3D (setting "Instant chain explosions"), so that
 * its dead fall after it?
 * @param state The battlescape screen.
 * @return True while the battle waits.
 */
bool chainRippleHolds(BattlescapeState *state)
{
	return FirstPersonMode::chainRippleHolds(state);
}

/**
 * Called by the game as it fires a shotgun's round or an akimbo volley: does each pellet (and each
 * gun's round) land on its own (setting "Each pellet on its own"), rather than as the lead pellet
 * lands, the rest lost with it when it leaves the map?
 * @param state The battlescape screen.
 * @return True if each lands on its own.
 */
bool eachPellet(BattlescapeState *state)
{
	return FirstPersonMode::eachPellet(state);
}

/**
 * Called by the game as it fills the map's stat bars (the 3D view's are filled the same way): the scale to draw one at (see
 * FirstPersonMode::statBarScale).
 * @param state The battlescape screen.
 * @param stat Which stat (StatBar).
 * @param maximum The unit's maximum of it.
 * @param barWidth The bar's width, in pixels.
 * @return Pixels per point.
 */
double statBarScale(BattlescapeState *state, int stat, int maximum, int barWidth)
{
	return FirstPersonMode::statBarScale(state, stat, maximum, barWidth);
}

/**
 * Called by the game as a hand's weapon is clicked with Ctrl and Shift held: akimbo on the map
 * (see FirstPersonMode::toggleMapAkimbo).
 * @param state The battlescape screen.
 * @param unit The selected soldier.
 * @return True if it took the click (the game then does nothing else with it).
 */
bool toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit)
{
	return FirstPersonMode::toggleMapAkimbo(state, unit);
}

/**
 * Called by the game as a reaction shot is launched (TileEngine::tryReaction): with the soldier's
 * akimbo marked (on the map, or by the akimbo key in 3D), the other hand's gun fires along (see
 * FirstPersonMode::reactionAkimbo).
 * @param state The battlescape screen.
 * @param action The reaction shot.
 */
void reactionAkimbo(BattlescapeState *state, BattleAction &action)
{
	FirstPersonMode::reactionAkimbo(state, action);
}

/**
 * Called by the game after reaction fire came at a unit (TileEngine::checkReactionFire, when a
 * reactor shot at it or struck it): in 3D, the soldier's walk stops (see
 * FirstPersonMode::firedAt), as the game stops a walk on the map.
 * @param state The battlescape screen.
 * @param unit The unit fired at.
 */
void reactionFiredAt(BattlescapeState *state, const BattleUnit *unit)
{
	FirstPersonMode::firedAt(state, unit);
}

/**
 * Called by the game after a round or a blow struck a unit (TileEngine::hit): one that did no
 * harm to an enemy in sight marks the 3D view's crosshair (see FirstPersonMode::unitStruck).
 * @param state The battlescape screen.
 * @param unit The unit struck.
 * @param healthBefore Its health before the strike.
 * @param stunBefore Its stun before the strike.
 */
void unitStruck(BattlescapeState *state, const BattleUnit *unit, int healthBefore, int stunBefore)
{
	FirstPersonMode::unitStruck(state, unit, healthBefore, stunBefore);
}

/**
 * Called by the game as a shot, a blow or a blast has hurt a unit (TileEngine::hitUnit): setting
 * "Damage numbers" (see FirstPersonMode::unitDamaged).
 * @param state The battlescape screen (null while the battle is being made).
 * @param unit The unit hurt.
 * @param healthLost The health it lost.
 * @param armorLost The armour points it lost (the side struck).
 * @param stunTaken The stun it took.
 * @param shieldLost The points its energy shield lost (see shieldPoints).
 * @param healthDealt The health the hit took before the game's floor (see noteHealthDamage).
 */
void unitDamaged(BattlescapeState *state, const BattleUnit *unit, int healthLost, int armorLost, int stunTaken, int shieldLost, int healthDealt)
{
	FirstPersonMode::unitDamaged(state, unit, healthLost, armorLost, stunTaken, shieldLost, healthDealt);
}

/**
 * Setting "Obliteration": a unit beyond the game's overkill bursts in 3D (see FirstPersonApi.h, FirstPersonMode::unitObliterated).
 */
void unitObliterated(BattlescapeState *state, BattleUnit *unit, const Position &relative, const RuleDamageType *type)
{
	FirstPersonMode::unitObliterated(state, unit, relative, type);
}

/**
 * Setting "Obliteration": a body on the floor a blast destroyed bursts in 3D (see FirstPersonApi.h, FirstPersonMode::itemBlasted).
 */
void itemBlasted(BattlescapeState *state, const BattleItem *item, const Position &center, const RuleDamageType *type)
{
	FirstPersonMode::itemBlasted(state, item, center, type);
}

/**
 * What the hit log adds after a hit's word with the setting "Detailed hit log" (see FirstPersonApi.h; the owner: the
 * original log "in 3D window exactly", enhanced inside its window, with who was hit, the damage numbers, and kills and
 * knock-outs). The game's own log says only 0, hit (up to a fifth of the unit's full health) or HIT for each unit a
 * hit hurts; this names the unit as the game names it (a soldier's name, an alien's race), the health it lost and the
 * stun it took, and "killed" or "down" when that hit put it out. A unit the squad doesn't see stays unnamed, with no
 * numbers ("unseen"): the log tells no more than the player could know (in the game's debug mode, everything).
 * @param state The battlescape screen.
 * @param target The unit hit.
 * @param healthDamage The health it lost.
 * @param stunDamage The stun it took.
 * @param wasOut It was down or dead already before the hit.
 * @return The text, ending in a space; empty with the setting off.
 */
std::string hitLogDetail(BattlescapeState *state, const BattleUnit *target, int healthDamage, int stunDamage, bool wasOut)
{
	if (!state || !target || !Options::fpsHitLogDetail)
	{
		return std::string();
	}
	Language *lang = state->getGame()->getLanguage();
	const SavedBattleGame *save = state->getGame()->getSavedGame() ? state->getGame()->getSavedGame()->getSavedBattle() : nullptr;
	const bool known = target->getOriginalFaction() == FACTION_PLAYER || target->getFaction() == FACTION_PLAYER || target->getVisible()
		|| (save && save->getDebugMode());
	std::string text = "(";
	if (!known)
	{
		text += std::string(lang->getString("STR_FPS_HIT_LOG_UNSEEN"));
	}
	else
	{
		text += target->getName(lang);
		if (healthDamage > 0) text += " " + std::to_string(healthDamage);
		if (stunDamage > 0) text += std::string(healthDamage > 0 ? ", " : " ") + std::string(lang->getString("STR_FPS_HIT_LOG_STUN")) + " " + std::to_string(stunDamage);
		if (!wasOut && target->isOutThresholdExceed())
		{
			text += ", " + std::string(lang->getString(target->getHealth() <= 0 ? "STR_FPS_HIT_LOG_KILLED" : "STR_FPS_HIT_LOG_DOWN"));
		}
	}
	return text + ") ";
}

namespace
{
/// The health the last hit took before the game's floor (see noteHealthDamage).
int healthDamageNoted = 0;
}

/**
 * Notes the health a hit takes off a unit as the game works it out, before it floors the unit's health (most of
 * the game's damage types stop it at 0: a killing blow's health lost is what was left): setting "Damage numbers"
 * Overkill (a player, GitHub issue #262: "show the ACTUAL raw damage roll, to know how much overkill was done").
 * BattleUnit::damage notes it; TileEngine::hitUnit notes 0 before the hit (a hit that deals none notes nothing)
 * and reads it after.
 * @param health The health.
 */
void noteHealthDamage(int health)
{
	healthDamageNoted = health;
}

/// The health the last hit took before the game's floor (see noteHealthDamage).
int lastHealthDamage()
{
	return healthDamageNoted;
}

/// Setting "Blood trails": the most blood one unit leaves on one tile (a pool grows no bigger), and the most places kept in a
/// battle (the oldest go first).
const int BloodAmountMost = 40;
const size_t BloodPlacesMost = 3000;

/**
 * Setting "Blood trails": a unit with fatal wounds leaves blood on the floor, as much as its wounds (the owner: "put a puddle
 * of blood where unit is standing and tiles unit walks over. If unit is patched bleeding stops"): each step finished, a drop
 * for each wound (at most three); as its side's turn starts, twice that for each wound (at most five) where it stays, so a pool
 * grows under one that doesn't move. A body out cold bleeds where it lies (not while it is carried). The blood falls through a
 * floorless tile to the floor below. Every unit's that bleeds by the game's rules (BattleUnit::getFatalWounds; robots,
 * tanks and the bleed-immune never have wounds), seen or not: it lies there to be found (the owner's ruling: track by blood).
 * Kept in the save (SavedBattleGame::getBlood); the 3D view draws it (FirstPersonView::drawBlood). Nothing with the setting off.
 * @param save The battle.
 * @param unit The unit.
 * @param turn True as its side's turn starts, false for a step.
 */
void bleed(SavedBattleGame *save, BattleUnit *unit, bool turn)
{
	if (!Options::fpsBloodTrails || !save || !unit || unit->getStatus() == STATUS_DEAD || unit->getStatus() == STATUS_IGNORE_ME
		|| unit->getFatalWounds() <= 0)
	{
		return;
	}
	Tile *tile = unit->getTile();
	if (unit->isOut())
	{
		tile = nullptr;
		for (const BattleItem *item : *save->getItems())
		{
			if (item->getUnit() == unit)
			{
				tile = item->getTile();
				break;
			}
		}
	}
	while (tile && tile->getPosition().z > 0 && tile->hasNoFloor(save))
	{
		tile = save->getTile(tile->getPosition() + Position(0, 0, -1));
	}
	if (!tile)
	{
		return;
	}
	const int amount = turn ? 2 * std::min(unit->getFatalWounds(), 5) : std::min(unit->getFatalWounds(), 3);
	const int index = save->getTileIndex(tile->getPosition());
	std::vector<int> &blood = save->getBlood();
	for (size_t at = 0; at + 2 < blood.size(); at += 3)
	{
		if (blood[at] == index && blood[at + 1] == unit->getId())
		{
			blood[at + 2] = std::min(BloodAmountMost, blood[at + 2] + amount);
			return;
		}
	}
	if (blood.size() >= BloodPlacesMost * 3)
	{
		blood.erase(blood.begin(), blood.begin() + 3);
	}
	blood.insert(blood.end(), { index, unit->getId(), amount });
}

/**
 * The points left in a unit's energy shields (setting "Damage numbers"; a player's report, GitHub
 * issue #133: a hit a shield took whole showed a 0). The big mods' shields (X-Com Files,
 * XPiratez) are made by one script, which keeps a shield's points in a tag on the unit (an
 * armor's shield) and on an item (a shield held), and takes a hit off the left hand's, the right
 * hand's and then the armor's: the three together are read here, before and after a hit, as the
 * armour is. A game without those tags has no such shields: 0.
 * @param state The battlescape screen (null while the battle is being made).
 * @param unit The unit.
 * @return The points.
 */
int shieldPoints(BattlescapeState *state, const BattleUnit *unit)
{
	if (!state || !Options::fpsDamageNumbers)
	{
		return 0;
	}
	const ScriptGlobal *scripts = state->getGame()->getMod()->getScriptGlobal();
	const auto unitTag = scripts->getTag<ScriptValues<BattleUnit>::Tag>("Tag.UNIT_ENERGY_SHIELD_HP");
	const auto itemTag = scripts->getTag<ScriptValues<BattleItem>::Tag>("Tag.ITEM_ENERGY_SHIELD_HP");
	int points = unitTag ? unit->getScriptValues().get(unitTag) : 0;
	if (itemTag)
	{
		for (const BattleItem *held : { unit->getLeftHandWeapon(), unit->getRightHandWeapon() })
		{
			if (held) points += held->getScriptValues().get(itemTag);
		}
	}
	return points;
}

/**
 * A unit's armour, all its sides' points together, as they are now (the game lowers the side a hit
 * strikes): read by the game before and after a hit for setting "Damage numbers".
 * @param unit The unit.
 * @return The points.
 */
int armorPoints(const BattleUnit *unit)
{
	int points = 0;
	for (int side = 0; side < SIDE_MAX; ++side)
	{
		points += unit->getArmor((UnitSide)side);
	}
	return points;
}

/**
 * Called by the game as it finishes drawing the isometric map: setting "Damage numbers" (see
 * FirstPersonMode::drawMapDamageNumbers).
 * @param state The battlescape screen.
 * @param surface The map's surface (locked).
 * @param camera The map's camera.
 */
void drawMapDamageNumbers(BattlescapeState *state, Surface *surface, const Camera *camera)
{
	FirstPersonMode::drawMapDamageNumbers(state, surface, camera);
}

/**
 * Called by the game after it draws the hands' weapons: akimbo on the map's mini targets (see
 * FirstPersonMode::drawMapAkimbo).
 * @param state The battlescape screen.
 * @param unit The selected soldier (null: none the player can use).
 * @param leftHand The left hand's box.
 * @param rightHand The right hand's box.
 */
void drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand)
{
	FirstPersonMode::drawMapAkimbo(state, unit, leftHand, rightHand);
}

namespace
{
unsigned &aimCallOffCount()
{
	static unsigned count = 0;
	return count;
}
}

/**
 * The game is ending (see FirstPersonApi.h).
 */
void closeGpu()
{
	closeFirstPersonGpu();
}

/**
 * The game's aim was called off (see FirstPersonApi.h).
 */
void aimCalledOff()
{
	++aimCallOffCount();
}

/**
 * How many times the game's aim was called off (see FirstPersonApi.h).
 */
unsigned aimCallOffs()
{
	return aimCallOffCount();
}

/**
 * Called by the game as it launches a shot clicked on the map: akimbo on the map (see
 * FirstPersonMode::mapAkimbo).
 * @param state The battlescape screen.
 * @param action The shot.
 */
void mapAkimbo(BattlescapeState *state, BattleAction &action)
{
	FirstPersonMode::mapAkimbo(state, action);
}

/**
 * A shot or a throw fired at its target: fired without the game's turn first (see
 * FirstPersonMode::shotKeepsFacing)?
 */
bool shotKeepsFacing(BattlescapeState *state, const BattleAction &action)
{
	return FirstPersonMode::shotKeepsFacing(state, action);
}

/**
 * A shotgun's pellet landed: heard, and marked in the 3D view (see FirstPersonMode::pelletLanded).
 * @param state The battlescape screen.
 * @param at Where it struck, in voxels.
 * @param from A point further back on its path, for its direction.
 * @param impact What it struck (a VoxelType; V_OUTOFBOUNDS: nothing).
 * @param weapon The weapon that fired it.
 * @param ammo Its ammo.
 * @param heard Whether it's heard (not one whose hit the explosion state brings, with its own sound).
 */
void pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard)
{
	FirstPersonMode::pelletLanded(state, at, from, impact, weapon, ammo, heard);
}

/**
 * A round (or a thrown item) took a step along its path: glass in a window's opening there
 * shatters in the 3D view (see FirstPersonMode::roundPassed).
 * @param state The battlescape screen.
 * @param voxel Where it is now, in voxels.
 * @param from Where it was a few steps back, for its way.
 */
void roundPassed(BattlescapeState *state, const Position &voxel, const Position &from)
{
	FirstPersonMode::roundPassed(state, voxel, from);
}

/**
 * A blast that harms terrain reached these tiles: the glass in the openings of their windows
 * shatters in the 3D view (see FirstPersonMode::blastReached).
 * @param state The battlescape screen.
 * @param center Where it went off, in voxels.
 * @param tiles The tiles it reached, with the damage it does to each.
 */
void blastReached(BattlescapeState *state, const Position &center, const std::map<Tile*, int> &tiles)
{
	FirstPersonMode::blastReached(state, center, tiles);
}

/**
 * The bullet sprite for shotgun ammo that has none (a player's report: on the isometric map a
 * shotgun's round is "unsprited"; "Just using the most common used sprite per that damage type
 * should be fitting for now"): of the mod's items with a bullet sprite, the one used most by those
 * of the ammo's damage type, or by all of them when none shares it. Worked out once per mod.
 * @param mod The mod.
 * @param ammo The shotgun ammo.
 * @return The bullet sprite, or Mod::NO_SURFACE when no item has one.
 */
int shotgunBulletSprite(const Mod *mod, const RuleItem *ammo)
{
	// The most used sprite per damage type (and, under DT_NONE's key -1, overall), per mod.
	static const Mod *countedFor = nullptr;
	static std::map<int, int> mostUsed;
	if (countedFor != mod)
	{
		countedFor = mod;
		mostUsed.clear();
		std::map<int, std::map<int, int>> votes; // damage type (-1: all) -> sprite -> items using it
		for (const std::string &name : mod->getItemsList())
		{
			const RuleItem *item = mod->getItem(name);
			if (item && item->getBulletSprite() != Mod::NO_SURFACE && item->getDamageType())
			{
				++votes[item->getDamageType()->ResistType][item->getBulletSprite()];
				++votes[-1][item->getBulletSprite()];
			}
		}
		for (const auto &[damage, sprites] : votes)
		{
			mostUsed[damage] = std::max_element(sprites.begin(), sprites.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
		}
	}
	auto found = mostUsed.find(ammo->getDamageType() ? (int)ammo->getDamageType()->ResistType : -1);
	if (found == mostUsed.end()) found = mostUsed.find(-1);
	return found != mostUsed.end() ? found->second : Mod::NO_SURFACE;
}

/**
 * Called by the game when a unit panics or goes berserk: the first-person view may show it its
 * own way (for the player's soldiers in the player's turn), and the game then leaves out its
 * little "... is panicking" infobox.
 * @param state The battlescape screen.
 * @param unit The unit breaking down.
 * @return True if the first-person view shows it.
 */
bool breakdown(BattlescapeState *state, BattleUnit *unit)
{
	return FirstPersonMode::breakdown(state, unit);
}

/**
 * Called by the game whenever a weapon would spend ammo: with Doom IDKFA on, the selected
 * soldier's shots in the 3D view spend none, so even a one-round clip (a rocket) isn't used up.
 * @param state The battlescape screen.
 * @param unit The unit holding the weapon.
 * @return True if the shot spends no ammo.
 */
bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit)
{
	return FirstPersonMode::endlessAmmo(state, unit);
}

/**
 * Called by the game whenever a unit is about to take damage: with Doom IDDQD on, X-COM's units
 * take none.
 * @param state The battlescape screen.
 * @param unit The unit about to be hurt.
 * @return True if it takes no damage.
 */
bool invincible(BattlescapeState *state, const BattleUnit *unit)
{
	return FirstPersonMode::invincible(state, unit);
}

/**
 * Called by the game as a unit is about to start falling: does it float instead (Doom IDCLIP)?
 * @param state The battlescape screen.
 * @param unit The unit with no floor under it.
 * @return True if it stays where it is.
 */
bool noclipFloats(BattlescapeState *state, const BattleUnit *unit)
{
	return FirstPersonMode::noclipFloats(state, unit);
}

/**
 * Called by the game as its map screen is asked for (the map's button, its key): the tactical map instead, from the
 * 3D view with the setting on (see FirstPersonMode::openTacticalMap).
 * @param state The battlescape screen.
 * @return True if the tactical map was opened: the game's own stays shut.
 */
bool tacticalMap(BattlescapeState *state)
{
	return FirstPersonMode::openTacticalMap(state);
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
