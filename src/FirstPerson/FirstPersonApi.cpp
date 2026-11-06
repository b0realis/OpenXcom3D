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
#include <algorithm>
#include <map>
#include "FirstPersonMode.h"
#include "OptionsFirstPersonState.h"
#include "../Engine/Game.h"
#include "../Engine/OptionInfo.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"
#include "../Engine/State.h"
#include "../Interface/Text.h"
#include "FirstPersonModeShared.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/BattleItem.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/**
 * Whether a battle with the mod's 3D view is on: its mode made (see create) and not yet
 * destroyed. For the rule fixes that come with the mod (see keepsFacingOnOwnTile), which units
 * ask without a way to the battle's screen.
 * @return The flag.
 */
bool &battleWithView()
{
	static bool on = false;
	return on;
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
	battleWithView() = true;
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
		battleWithView() = false;
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
	return battleWithView();
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
 * The main menu with the mod on: its version (the mod's own metadata, "1.11") in small letters
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
 * flashlight (Off, On, Dynamic) from 1.0's "Flashlight" and "Tactical flashlight"; akimbo (Off,
 * Full, Realistic) from 1.08's "Full akimbo" and "Realistic akimbo" (and with akimbo Off, those
 * two, which now tell whether it's on just now, off: a hand-edited profile can't have two guns
 * fire with the setting Off, the akimbo key refusing to switch them off); and 1.0's next fire
 * mode key, when the player had moved it off F (F is the flashlight now), once.
 */
void settleOptions()
{
	settlePlaystyle();
	Options::fpsSmoke = smokeMode();
	Options::fpsFlashlightMode = flashlightMode();
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
}

namespace
{

/**
 * One of the switches the setting "Playstyle" sets: its option (a switch or a number), and its
 * value under Faithful (the game's own way, the defaults) and under FPS (a shooter's).
 */
struct PlaystyleSwitch
{
	const char *id;
	bool *flag;
	int *number;
	int faithful, fps;
};

/**
 * The switches the setting "Playstyle" sets (the owner's choice: shooting and turning, moving,
 * the feel of combat; the cheats stay apart): FPS-style shooting, turning by the rules, movement
 * tile by tile, jumping over low obstacles, leaning (Peek) and lean shots, full sprite hitboxes,
 * quick deaths, melee hitting walls and the downed, and the live motion scanner.
 * @return The switches.
 */
const std::vector<PlaystyleSwitch> &playstyleSwitches()
{
	static const std::vector<PlaystyleSwitch> switches = {
		{ "fpsFaithfulTargeting", &Options::fpsFaithfulTargeting, nullptr, 1, 0 },
		{ "fpsTurnCost", &Options::fpsTurnCost, nullptr, 1, 0 },
		{ "fpsMovement", nullptr, &Options::fpsMovement, 1, 0 },
		{ "fpsVault", &Options::fpsVault, nullptr, 0, 1 },
		{ "fpsLean", nullptr, &Options::fpsLean, 0, 1 },
		{ "fpsLeanShots", &Options::fpsLeanShots, nullptr, 0, 1 },
		{ "fpsSpriteHitboxes", &Options::fpsSpriteHitboxes, nullptr, 0, 1 },
		{ "fpsQuickDeaths", &Options::fpsQuickDeaths, nullptr, 0, 1 },
		{ "fpsMeleeTerrain", &Options::fpsMeleeTerrain, nullptr, 0, 1 },
		{ "fpsLiveScanner", &Options::fpsLiveScanner, nullptr, 0, 1 },
	};
	return switches;
}

/**
 * A playstyle switch's value now (a switch as 0 or 1).
 * @param playstyleSwitch The switch.
 * @return The value.
 */
int switchValue(const PlaystyleSwitch &playstyleSwitch)
{
	return playstyleSwitch.flag ? (*playstyleSwitch.flag ? 1 : 0) : *playstyleSwitch.number;
}

/**
 * Sets a playstyle switch.
 * @param playstyleSwitch The switch.
 * @param value Its value (a switch: 0 or not).
 */
void setSwitch(const PlaystyleSwitch &playstyleSwitch, int value)
{
	if (playstyleSwitch.flag) *playstyleSwitch.flag = value != 0;
	else *playstyleSwitch.number = value;
}

/**
 * The playstyle switches as they are now, as Custom keeps them: "fpsTurnCost=0;fpsMovement=0;...".
 * @return The text.
 */
std::string currentMix()
{
	std::string mix;
	for (const PlaystyleSwitch &playstyleSwitch : playstyleSwitches())
	{
		mix += std::string(playstyleSwitch.id) + "=" + std::to_string(switchValue(playstyleSwitch)) + ";";
	}
	return mix;
}

/**
 * The playstyle the switches are in now: Faithful (0) if all of them are as Faithful has them,
 * FPS (1) if all are as FPS has them, else Custom (2).
 * @return The playstyle.
 */
int playstyleOfSwitches()
{
	bool faithful = true, fps = true;
	for (const PlaystyleSwitch &playstyleSwitch : playstyleSwitches())
	{
		faithful = faithful && switchValue(playstyleSwitch) == playstyleSwitch.faithful;
		fps = fps && switchValue(playstyleSwitch) == playstyleSwitch.fps;
	}
	return faithful ? 0 : fps ? 1 : 2;
}

}

/**
 * Chooses a playstyle (setting "Playstyle", the owner's ask: "Faithful has all that we have now
 * defaults (close to iso play but in 3d). Doom mode pushes multiple switches for easier FPS
 * gameplay. And third setting custom - it is a mix of player own settings and remembers player
 * settings in next start"): Faithful and FPS set their switches (see playstyleSwitches); Custom
 * sets them back as the player had them (Options::fpsCustomPlaystyle). Leaving Custom keeps the
 * mix first, so flipping to Faithful or FPS and back loses nothing.
 * @param playstyle Faithful (0), FPS (1) or Custom (2).
 */
void choosePlaystyle(int playstyle)
{
	if (Options::fpsPlaystyle == 2)
	{
		Options::fpsCustomPlaystyle = currentMix();
	}
	Options::fpsPlaystyle = std::clamp(playstyle, 0, 2);
	for (const PlaystyleSwitch &playstyleSwitch : playstyleSwitches())
	{
		if (Options::fpsPlaystyle < 2)
		{
			setSwitch(playstyleSwitch, Options::fpsPlaystyle == 0 ? playstyleSwitch.faithful : playstyleSwitch.fps);
			continue;
		}
		// Custom: the kept value, where there is one ("id=value;").
		const std::string key = std::string(playstyleSwitch.id) + "=";
		const size_t at = Options::fpsCustomPlaystyle.find(key);
		if (at != std::string::npos && (at == 0 || Options::fpsCustomPlaystyle[at - 1] == ';'))
		{
			setSwitch(playstyleSwitch, std::atoi(Options::fpsCustomPlaystyle.c_str() + at + key.size()));
		}
	}
}

/**
 * Notes a playstyle switch changed by hand (setting "Playstyle"): the playstyle becomes the one
 * the switches now make (Faithful or FPS if they are all as those have them), else Custom,
 * keeping the mix.
 */
void playstyleSwitchChanged()
{
	Options::fpsPlaystyle = playstyleOfSwitches();
	if (Options::fpsPlaystyle == 2)
	{
		Options::fpsCustomPlaystyle = currentMix();
	}
}

/**
 * Whether a setting is one the setting "Playstyle" sets (see playstyleSwitches).
 * @param id The setting's id.
 * @return True if so.
 */
bool isPlaystyleSwitch(const std::string &id)
{
	for (const PlaystyleSwitch &playstyleSwitch : playstyleSwitches())
	{
		if (id == playstyleSwitch.id) return true;
	}
	return false;
}

/**
 * Settles the setting "Playstyle" with the switches as they are (a profile from before it, its
 * switches the player's own, or one edited by hand): Faithful or FPS only if the switches are all
 * as that playstyle has them, else Custom, keeping them as the mix. Custom stays Custom.
 */
void settlePlaystyle()
{
	// The live motion scanner joined the playstyle in 1.11 (GitHub issue #101): a profile from before
	// it that says FPS gets it as FPS has it (switched off by hand, it would have said Custom).
	if (Options::fpsPlaystyle == 1 && !Options::fpsLiveScanner)
	{
		Options::fpsLiveScanner = true;
	}
	if (Options::fpsPlaystyle != 2 && playstyleOfSwitches() != Options::fpsPlaystyle)
	{
		Options::fpsPlaystyle = 2;
		Options::fpsCustomPlaystyle = currentMix();
	}
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
 * The flashlight setting as it stands: Off (0), On (1) or Dynamic (2). Unset (-1: a profile from
 * 1.0 not settled yet, or the defaults just restored on the 3D VIEW tab), it's what 1.0's two
 * settings say: "Tactical flashlight" YES Dynamic, else "Flashlight" YES On, NO Off.
 * @return 0, 1 or 2.
 */
int flashlightMode()
{
	if (Options::fpsFlashlightMode >= 0 && Options::fpsFlashlightMode <= 2)
	{
		return Options::fpsFlashlightMode;
	}
	return Options::fpsTacticalFlashlight ? 2 : (Options::fpsFlashlight ? 1 : 0);
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
	// The playstyle (the owner's ask: a theme flipping the switches that make it play like the
	// game or like a shooter, see choosePlaystyle): Faithful (0), FPS (1) or Custom (2), the
	// player's own mix of those switches kept (unlisted: no description).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPlaystyle", &fpsPlaystyle, 0, "STR_FPS_PLAYSTYLE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCustomPlaystyle", &fpsCustomPlaystyle, "", "", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSoldierPreview", &fpsSoldierPreview, true, "STR_FPS_SOLDIER_PREVIEW", category));
	// And on the isometric map, the selected soldier's helmet camera in a window: off, small,
	// medium or large (0 to 3).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMapCamera", &fpsMapCamera, 0, "STR_FPS_MAP_CAMERA", category));
	// The helmet cameras see as the soldier's eyes do (a player, GitHub issue #90: an X-Com Files
	// X-Bat's heat vision saw through smoke, its camera didn't): their heat vision through smoke
	// and fire. Off (the owner: a realistic camera, as before, by default): a camera hasn't the
	// wearer's eyes.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCameraEyes", &fpsCameraEyes, false, "STR_FPS_CAMERA_EYES", category));
	// Mouse look: how fast the view turns (1 to 20), up and down swapped or not, and the field
	// of view across the screen, in degrees (50 to 110).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMouseSensitivity", &fpsMouseSensitivity, 5, "STR_FPS_MOUSE_SENSITIVITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsInvertMouse", &fpsInvertMouse, false, "STR_FPS_INVERT_MOUSE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFieldOfView", &fpsFieldOfView, 75, "STR_FPS_FIELD_OF_VIEW", category));
	// Zoom: holding the zoom key (or the right button in mouse look) looks 4 times closer.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsZoom", &fpsZoom, true, "STR_FPS_ZOOM", category));
	// The middle button as on the isometric map: a click on a unit in sight shows it (the game's
	// unit screen); anywhere else it frees the cursor, as always.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMiddleClickIdentify", &fpsMiddleClickIdentify, true, "STR_FPS_MIDDLE_CLICK_IDENTIFY", category));
	// Turning: free, the soldier facing wherever you look (NO), or by the game's rules (YES): the
	// head looks up to 45 degrees off the soldier's facing for free, further turns them at the
	// game's cost. YES by default (a player, GitHub issue #63: "It being off, is basically a cheat";
	// the defaults are the game's own mechanics).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTurnCost", &fpsTurnCost, true, "STR_FPS_TURN_COST", category));
	// The compass at the top right: where you look, the soldier's facing, and turning by the
	// rules, when looking further will cost time units.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCompass", &fpsCompass, true, "STR_FPS_COMPASS", category));
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
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFalling", &fpsFalling, true, "STR_FPS_FALLING", category));
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
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAutoEquipGunsFirst", &fpsAutoEquipGunsFirst, false, "STR_FPS_AUTO_EQUIP_GUNS_FIRST", category));
	// Leaning: standing to one side of the tile's middle shoots from that side (the game's
	// off-centre origins), to peek round corners; and leaning out with the lean keys: off (0),
	// the eye moves to the side (1, peek), and the view tilts with it (2, tilt).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLeanShots", &fpsLeanShots, false, "STR_FPS_LEAN_SHOTS", category));
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
	// The flashlight, a light of the 3D view's own (on and off with its key, F; the suit light
	// stays the game's): none (0), a wide beam from the shoulder in the dark (1, on), or a light
	// on the weapon, a tight spot on the crosshair at any time of day (2, dynamic). Unset (-1)
	// until settled (see settleOptions) from 1.0's two settings, kept below, unlisted (a new name,
	// as 1.0's YES/NO wouldn't load into a number).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlightMode", &fpsFlashlightMode, -1, "STR_FPS_FLASHLIGHT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionGear", &fpsNightVisionGear, false, "STR_FPS_NIGHT_VISION_GEAR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCombatMusic", &fpsCombatMusic, true, "STR_FPS_COMBAT_MUSIC", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitSounds", &fpsHitSounds, true, "STR_FPS_HIT_SOUNDS", category));
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
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlares", &fpsFlares, true, "STR_FPS_FLARES", category));
	// 0.9's "See-through smoke" (YES/NO), loaded and saved but listed nowhere (a category no
	// options screen shows): it gives the smoke setting its first value.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughSmoke", &fpsSeeThroughSmoke, true, "STR_FPS_SEE_THROUGH_SMOKE", "STR_FIRST_PERSON_SAVED"));
	// 1.0's "Flashlight" and "Tactical flashlight" (YES/NO), the same way: they give the
	// flashlight setting its first value.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlight", &fpsFlashlight, true, "STR_FPS_FLASHLIGHT", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTacticalFlashlight", &fpsTacticalFlashlight, false, "STR_FPS_FLASHLIGHT", "STR_FIRST_PERSON_SAVED"));
	// Looks: the weapon in hand (always made of voxels), sprite upscaling (0 off, 1 smooth,
	// 2 smooth + detail, 3 finest: six times bigger; the default, a profile saved before keeps its
	// value), muzzle flashes, particles (0 none, 1 effects, 2 everything),
	// UFOs with steady hulls, glowing lights, coloured light and an aura, and the pixel size
	// (1 to 3: one ray colours a block of pixels that wide and high, for speed).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewModel", &fpsViewModel, true, "STR_FPS_VIEW_MODEL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteUpscale", &fpsSpriteUpscale, 3, "STR_FPS_SPRITE_UPSCALE", category));
	// Soldiers' and aliens' sprites a little smaller (80 to 100% of the isometric map's size).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitSize", &fpsUnitSize, 100, "STR_FPS_UNIT_SIZE", category));
	// Units (one tile big or two by two) as 3D bodies made from their own sprites (see VoxelUnits; the owner's sub-project).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVoxelUnits", &fpsVoxelUnits, false, "STR_FPS_VOXEL_UNITS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMuzzleFlash", &fpsMuzzleFlash, true, "STR_FPS_MUZZLE_FLASH", category));
	// A unit hit, standing or down, flashes for a moment (the owner, on GitHub issue #74: its own
	// setting); off, it only flinches. "Muzzle flashes" off turns it off too (every flash: the
	// setting for players whom flashing light bothers).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitFlash", &fpsHitFlash, true, "STR_FPS_HIT_FLASH", category));
	// Screen shake (a player, GitHub issue #67): off, nothing shakes the view (explosions, hits,
	// falls, landings, panic, one's own shots).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsScreenShake", &fpsScreenShake, true, "STR_FPS_SCREEN_SHAKE", category));
	// Futuristic night vision (a player, GitHub issue #88: the map's night vision leaves units and
	// items their own colours): units, bodies and items keep theirs, the world goes green. Off (the
	// owner: realistic, by default), everything goes green.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionUnits", &fpsNightVisionUnits, false, "STR_FPS_NIGHT_VISION_UNITS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsParticles", &fpsParticles, 2, "STR_FPS_PARTICLES", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUfoLights", &fpsUfoLights, true, "STR_FPS_UFO_LIGHTS", category));
	// Parts the map draws but gives no shape (no voxels: ladders, lifts' pads, walls of laser
	// beams), shown as see-through ghosts of their art: 0 none, 1 lifts and ladders (the parts
	// with the grav lift flag), 2 all of them.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostParts", &fpsGhostParts, 1, "STR_FPS_GHOST_PARTS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostOpacity", &fpsGhostOpacity, 50, "STR_FPS_GHOST_OPACITY", category));
	// The crops, flower beds and rubble the game gives no real shape, drawn solid as on the map (see
	// buildRelief, buildCropBlock): a bit see-through instead (SeeThroughCropOpacity of the pixels),
	// so whoever stands in a field shows (the owner's ask, apart from the ghosts' opacity).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughCrops", &fpsSeeThroughCrops, false, "STR_FPS_SEE_THROUGH_CROPS", category));
	// A corpse or item pointed at, the game's line passing it to something behind: drawn faint, at
	// the ghost opacity (the game's shots pass through them).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughCorpses", &fpsSeeThroughCorpses, false, "STR_FPS_SEE_THROUGH_CORPSES", category));
	// The map's edge as small red dots on the floor, nothing beyond it, instead of the tape.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDiscreetEdge", &fpsDiscreetEdge, false, "STR_FPS_DISCREET_EDGE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPixelSize", &fpsPixelSize, 0, "STR_FPS_PIXEL_SIZE", category));
	// The crosshair: shown or not, the shot info under it, aiming at the exact point under it
	// (YES) or at the tile, as the original game does (NO), and the units' hitboxes.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshair", &fpsCrosshair, true, "STR_FPS_CROSSHAIR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairInfo", &fpsCrosshairInfo, true, "STR_FPS_CROSSHAIR_INFO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairCosts", &fpsCrosshairCosts, true, "STR_FPS_CROSSHAIR_COSTS", category));
	// The shot's damage range under the hit chance, as the map shows it with Alt (a player, GitHub
	// issue #50; the owner: a setting, off).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairDamage", &fpsCrosshairDamage, false, "STR_FPS_CROSSHAIR_DAMAGE", category));
	// The soldier cards show each soldier's little figure from the craft screen, when the mod draws
	// one for the armor (a player, GitHub issue #53; the owner: the cards only, a setting, on).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCraftIcons", &fpsCraftIcons, true, "STR_FPS_CRAFT_ICONS", category));
	// A line under the crosshair as wide as a shot strays to either side: with the game's own
	// targeting, as far as a successful accuracy roll sends it at that range; in FPS style, the spread.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAccuracyLine", &fpsAccuracyLine, true, "STR_FPS_ACCURACY_LINE", category));
	// The name of the unit under the crosshair (or the free cursor), as the game names it.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitNames", &fpsUnitNames, true, "STR_FPS_UNIT_NAMES", category));
	// How the stat bars read, on the map and in 3D (a player's report: a 300-HP unit's bar looked
	// full with a third of its health gone): the game's own (0: a pixel per point, cut off at the
	// bar's end), scaled (1: a stat shrinks for all of the player's units alike when a maximum is
	// longer than the bar, on the map and in 3D), or percentage (2: each bar full at its maximum).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStatBars", &fpsStatBars, 1, "STR_FPS_STAT_BARS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAimExact", &fpsAimExact, true, "STR_FPS_AIM_EXACT", category));
	// The path a throw or shot would take, drawn while aiming: off (0), throws and arcing
	// shots (1), every shot too (2); and confirm fire: the first click marks the target and
	// shows its path, a second click on it fires.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTrajectory", &fpsTrajectory, 1, "STR_FPS_TRAJECTORY", category));
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
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitboxes", &fpsHitboxes, 3, "STR_FPS_HITBOXES", category));
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
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDownedIcons", &fpsDownedIcons, 1, "STR_FPS_DOWNED_ICONS", category));
	// Staying in the 3D view during the alien turn, starting battles in it, and the debug cheat
	// "doom mode" (endless time units and energy), with its two extras, each doom mode plus
	// more: IDKFA (endless ammo) and IDDQD (invincible soldiers); in doom mode grenades go off in
	// real time (one click, a few seconds' fuse: the owner, "realtime grenade setting should be
	// always on with doom mode", where it was a setting of its own).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurn3D", &fpsAlienTurn3D, true, "STR_FPS_ALIEN_TURN_3D", category));
	// ...and in it, the view cuts to the soldier who sees the alien acting, or who fires back;
	// a soldier shot at, hit or taken over pops up their small camera.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurnCameras", &fpsAlienTurnCameras, true, "STR_FPS_ALIEN_TURN_CAMERAS", category));
	// ...and with them, a soldier of ours going down out of view pops up their camera (static).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFallenSoldierCamera", &fpsFallenSoldierCamera, true, "STR_FPS_FALLEN_SOLDIER_CAMERA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStartInFirstPerson", &fpsStartInFirstPerson, false, "STR_FPS_START_IN_FIRST_PERSON", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomMode", &fpsDoomMode, false, "STR_FPS_DOOM_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIdkfa", &fpsDoomIdkfa, false, "STR_FPS_DOOM_IDKFA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIddqd", &fpsDoomIddqd, false, "STR_FPS_DOOM_IDDQD", category));
	// Doom mode plus noclip (a player's request, GitHub issue #45): in 3D the soldier walks and
	// floats through walls, objects, doors and floors, and X-COM's units don't fall.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIdclip", &fpsDoomIdclip, false, "STR_FPS_DOOM_IDCLIP", category));
	// Doom mode's real-time grenades: armed as they're thrown, burning from leaving the hand (YES), or primed with a click and
	// burning in the hand (NO, "cooking");
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomPrimeOnThrow", &fpsDoomPrimeOnThrow, true, "STR_FPS_DOOM_PRIME_ON_THROW", category));
	// ...thrown freely at whatever the crosshair points at, bursting in the air when the fuse runs out in flight;
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomFreeGrenades", &fpsDoomFreeGrenades, false, "STR_FPS_DOOM_FREE_GRENADES", category));
	// ...and their fuse, in seconds (1 to 5; with prime on throw, FUSE in battle steps it too).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomGrenadeFuse", &fpsDoomGrenadeFuse, 3, "STR_FPS_DOOM_GRENADE_FUSE", category));

	// The add-on's own keys, in the order the Controls list shows them, with their default keys.
	// Switching views, freeing the cursor from mouse look, and showing the classic HUD.
	options.push_back(OptionInfo(OPTION_OTHER, "keyBattleFirstPerson", &keyBattleFirstPerson, SDLK_F8, "STR_FPS_KEY_TOGGLE_VIEW", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsMouseMode", &keyFpsMouseMode, SDLK_c, "STR_FPS_KEY_MOUSE_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsClassicHud", &keyFpsClassicHud, SDLK_h, "STR_FPS_KEY_CLASSIC_HUD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsHideHud", &keyFpsHideHud, SDLK_p, "STR_FPS_KEY_HIDE_HUD", category));
	// Walking and strafing (W, A, S, D).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsForward", &keyFpsForward, SDLK_w, "STR_FPS_KEY_FORWARD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsBack", &keyFpsBack, SDLK_s, "STR_FPS_KEY_BACK", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeLeft", &keyFpsStrafeLeft, SDLK_a, "STR_FPS_KEY_STRAFE_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeRight", &keyFpsStrafeRight, SDLK_d, "STR_FPS_KEY_STRAFE_RIGHT", category));
	// Turning and looking up and down with the keys (the arrows): the 3D view's own, not the map's
	// scroll keys it used before (a player, GitHub issue #81: with the map scrolled on W, A, S, D,
	// those keys walked and turned at once).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLookLeft", &keyFpsLookLeft, SDLK_LEFT, "STR_FPS_KEY_TURN_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLookRight", &keyFpsLookRight, SDLK_RIGHT, "STR_FPS_KEY_TURN_RIGHT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLookUp", &keyFpsLookUp, SDLK_UP, "STR_FPS_KEY_LOOK_UP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLookDown", &keyFpsLookDown, SDLK_DOWN, "STR_FPS_KEY_LOOK_DOWN", category));
	// Two ways of walking, each on and off with its key: sprinting (the game's run; Caps Lock,
	// Doom's own "always run" key) and en passant (walking on past enemies coming into sight, as
	// Shift does on the map; the key left of 1, as Shift is the lean key).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsSprint", &keyFpsSprint, SDLK_CAPSLOCK, "STR_FPS_KEY_SPRINT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsEnPassant", &keyFpsEnPassant, SDLK_BACKQUOTE, "STR_FPS_KEY_EN_PASSANT", category));
	// Fire modes (the next one, or snap, aimed and auto directly) and switching hands. The next
	// fire mode is J (the game's "reserve TUs for kneel", which keeps it on the map), as F is the
	// flashlight. (1.0 saved it as keyFpsNextMode, on F: the new name leaves F behind, and a key
	// the player had picked instead carries over, see settleOptions.)
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextFireMode", &keyFpsNextFireMode, SDLK_j, "STR_FPS_KEY_NEXT_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextMode", &keyFpsNextMode, SDLK_f, "STR_FPS_KEY_NEXT_MODE", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeSnap", &keyFpsModeSnap, SDLK_z, "STR_FPS_KEY_MODE_SNAP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAimed", &keyFpsModeAimed, SDLK_x, "STR_FPS_KEY_MODE_AIMED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAuto", &keyFpsModeAuto, SDLK_v, "STR_FPS_KEY_MODE_AUTO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsSwitchHand", &keyFpsSwitchHand, SDLK_g, "STR_FPS_KEY_SWITCH_HAND", category));
	// Akimbo on and off in battle (Y: one of the letters the battlescape leaves free).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsAkimbo", &keyFpsAkimbo, SDLK_y, "STR_FPS_KEY_AKIMBO", category));
	// Akimbo's style, both at once or in turn (setting "Akimbo in turn"): unbound unless the player binds one.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsAkimboStyle", &keyFpsAkimboStyle, SDLK_UNKNOWN, "STR_FPS_KEY_AKIMBO_STYLE", category));
	// Turning 45 degrees at a time, without stepping (comma and period: "<" and ">").
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsTurnLeft", &keyFpsTurnLeft, SDLK_COMMA, "STR_FPS_KEY_SNAP_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsTurnRight", &keyFpsTurnRight, SDLK_PERIOD, "STR_FPS_KEY_SNAP_RIGHT", category));
	// Leaning out (setting "Lean"): the lean key held with a strafe key (Left Shift and A or D),
	// so Q and E stay the game's hand keys; and a key of its own for each side, held, unbound
	// unless the player binds one. (0.9's own lean keys, Q and E, were saved as keyFpsLeanLeft
	// and keyFpsLeanRight: the new names leave those behind, so Q and E are the hands again.)
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanHold", &keyFpsLeanHold, SDLK_LSHIFT, "STR_FPS_KEY_LEAN_HOLD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanLeftOwn", &keyFpsLeanLeftOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanRightOwn", &keyFpsLeanRightOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_RIGHT", category));
	// Opening doors (O, as E is the game's "use right hand"), flares, flight, jumping, night
	// vision, going up and down a level, and doom mode.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsOpenDoor", &keyFpsOpenDoor, SDLK_o, "STR_FPS_KEY_OPEN_DOOR", category));
	// Grabbing the item underfoot into a free hand, in 3D and on the map (GitHub issue #54): no key until
	// the player gives it one (every letter is the game's or ours).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsGrabItem", &keyFpsGrabItem, SDLK_UNKNOWN, "STR_FPS_KEY_GRAB_ITEM", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlare", &keyFpsFlare, SDLK_t, "STR_FPS_KEY_FLARE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlight", &keyFpsFlight, SDLK_b, "STR_FPS_KEY_FLIGHT", category));
	// A tank's turret switch (a player's report: turning the view turned the whole tank, at its full
	// cost; the TURRET button on the top bar too): no key until the player gives it one.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsTurret", &keyFpsTurret, SDLK_UNKNOWN, "STR_FPS_KEY_TURRET", category));
	// The flashlight on and off (F, as in the first-person shooters; setting "Flashlight").
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlashlight", &keyFpsFlashlight, SDLK_f, "STR_FPS_KEY_FLASHLIGHT", category));
	// Space is the map's night vision hold key; in 3D it jumps, and night vision is gear (N).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsJump", &keyFpsJump, SDLK_SPACE, "STR_FPS_KEY_JUMP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNightVision", &keyFpsNightVision, SDLK_n, "STR_FPS_KEY_NIGHT_VISION", category));
	// Zoom, held (U: free in the game and the add-on).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsZoom", &keyFpsZoom, SDLK_u, "STR_FPS_KEY_ZOOM", category));
	// Which way the units in sight face, held (Left Alt, as the map's own Alt shows its unit
	// indicators): a ring with an arrow on the floor round each, over everything.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsShowFacing", &keyFpsShowFacing, SDLK_LALT, "STR_FPS_KEY_SHOW_FACING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsUp", &keyFpsUp, SDLK_PAGEUP, "STR_FPS_KEY_UP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsDown", &keyFpsDown, SDLK_PAGEDOWN, "STR_FPS_KEY_DOWN", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsDoomMode", &keyFpsDoomMode, SDLK_INSERT, "STR_FPS_KEY_DOOM_MODE", category));
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

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
