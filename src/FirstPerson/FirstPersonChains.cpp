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
 * FirstPersonChains.cpp: setting "Instant chain explosions" (a player, GitHub issue #61: "massive
 * chain explosives torture my cpu ... instantly calculate everything and give the final results";
 * GitHub issue #105: a pile of barrels made two explosions "instead of 1 big boom"; the owner: "one
 * big blast but explosions shown at each explosive and short delay between them to really
 * emphasize the chain drama"). In 3D the explosives a blast sets off (a grenade's, a rocket's, or a
 * barrel's that a shot set off) go up at once, the whole chain one event under that blast
 * (ExplosionBState::settleChain): what it destroys and kills is the game's own. The blast itself
 * plays as ever, sounding as the chain's big boom; the 3D views show the chain as one big blast
 * rippling out from it: a fireball at each explosive, in the order the game sets them off,
 * ChainRippleDelay apart, each a normal blast's sprites (fewer each when many go up); the wreckage
 * and fire of each tile come as the fireball nearest to it grows over it, the map as it was until
 * then; the units the chain takes down stand as they were until the ripple has passed, and the
 * battle waits for it (once, not blast by blast), so they fall after it. A blast that sets nothing
 * off is untouched.
 */
#include "FirstPersonChains.h"
#include <algorithm>
#include <climits>
#include <random>
#include "FirstPersonApi.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Explosion.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// A chain's explosives go up this far apart in its ripple, in ms (the owner: "explosions shown at
/// each explosive and short delay between them to really emphasize the chain drama"): a pile of
/// six to eight reads as one chain going up, not as a slow sequence.
const Uint32 ChainRippleDelay = 120;
/// The whole ripple lasts no longer than this, in ms: in a longer chain the explosives go up closer
/// together (the big mods' chemical towers go up in dozens).
const Uint32 ChainRippleLongest = 1500;
/// The most fireball sprites a ripple has in all: each explosive's blast as many as the game's own
/// (one for every 5 of its power), fewer each when many go up, but at least ChainLeastSprites.
const int ChainRippleSprites = 160;
const int ChainLeastSprites = 3;
/// A tile's wreckage and fire show this long after the fireball nearest to it starts, in ms: once
/// the fireball has grown over it.
const Uint32 ChainRevealLag = 150;
/// The battle goes on (the chain's dead fall) this long after the last explosive's fireball starts,
/// in ms: as it fades.
const Uint32 ChainRippleTail = 350;
/// One frame of a fireball's sprites lasts as long as the game's own blast's, in ms (ExplosionBState::init).
const Uint32 FireballFrameTime = BattlescapeState::DEFAULT_ANIM_SPEED / 2;

/// An explosive of the chain: where it went up, its power, and when its fireball comes (SDL ticks).
struct ChainLink
{
	Position centre;
	int power;
	Uint32 at;
};

/// One sprite of a fireball: where it is, its blast's first frame, when it starts (SDL ticks), and
/// its count among all the fireball sprites (in the order they start).
struct ChainSprite
{
	Position position;
	int firstFrame;
	Uint32 start;
	Uint32 serial;
};

/// A unit as it was when the chain went off: its facing, and whether the player saw it.
struct ChainUnit
{
	BattleUnit *unit;
	int direction;
	bool seen;
};

/// The chain shown: one at a time (one going off while another's ripple still passes takes over,
/// the other's wreckage showing at once), its sprites kept until their frames are over.
struct ChainPicture
{
	const SavedBattleGame *save = nullptr; // the battle (null: none yet)
	bool settled = true;                   // its picture worked out (it's over in the battle)
	Uint32 start = 0;                      // when it went off (SDL ticks)
	Uint32 end = 0;                        // when the battle goes on, its dead falling
	std::vector<ChainLink> links;          // its explosives, in the order the game set them off
	std::vector<ChainHeldTile> before;     // every tile as it was when it went off (until settled)
	std::vector<ChainUnit> standing;       // the units up when it went off (until settled)
	std::vector<int> entries;              // per tile: its entry in held, or -1
	std::vector<ChainHeldTile> held;       // the tiles it changed, as they were, until the ripple passes them
	std::vector<ChainUnit> downed;         // the units it took down, as they were
	std::vector<ChainSprite> sprites;      // the fireballs' sprites, in the order they start
	Uint32 serials = 0;                    // fireball sprites counted so far
};

/// The chain shown (see ChainPicture).
ChainPicture &chainPicture()
{
	static ChainPicture picture;
	return picture;
}

/**
 * Works out a chain's picture once it has settled in the battle: when each explosive's fireball
 * comes, the fireballs' sprites, the tiles it changed (held as they were until the fireball
 * nearest to them has grown over them) and the units it took down (standing until the ripple has
 * passed, when the battle goes on).
 * @param chain The chain.
 * @param save Its battle.
 */
void settleChain(ChainPicture &chain, SavedBattleGame *save)
{
	chain.settled = true;
	if (chain.links.empty())
	{
		chain.end = chain.start;
		return;
	}

	// When each explosive's fireball comes: ChainRippleDelay apart, the first one after the blast
	// that set them off (the game's own, at the start), closer together in a chain so long that its
	// ripple would last more than ChainRippleLongest. The battle goes on once the last one fades
	// (ChainRippleTail).
	const int linkCount = (int)chain.links.size();
	const Uint32 delay = std::min(ChainRippleDelay, ChainRippleLongest / (Uint32)linkCount);
	for (int link = 0; link < linkCount; ++link)
	{
		chain.links[link].at = chain.start + delay * (link + 1);
	}
	chain.end = chain.links.back().at + ChainRippleTail;

	// The fireballs: each explosive's blast as the game scatters it (ExplosionBState::init): a sprite
	// for every 5 of its power, each up to half its power (in voxels) off its centre across, a frame
	// later for every fifth of them; fewer each when many go up (ChainRippleSprites in all). The
	// game's blast frames (X1.PCK; under the sea the ones before them). By the view's own dice: the
	// battle's own are left alone.
	std::minstd_rand dice(chain.start + 1);
	const int mostEach = std::max(ChainLeastSprites, ChainRippleSprites / linkCount);
	const int firstFrame = Mod::EXPLOSION_OFFSET - (save->getDepth() > 0 ? Explosion::EXPLODE_FRAMES : 0);
	std::vector<ChainSprite> sprites;
	for (const ChainLink &link : chain.links)
	{
		const int count = std::clamp(link.power / 5, 1, mostEach);
		const int perStep = std::max(1, count / 5);
		const int reach = std::max(1, link.power / 2);
		std::uniform_int_distribution<int> across(-reach, reach);
		for (int sprite = 0; sprite < count; ++sprite)
		{
			const Position offset(across(dice), across(dice), 0);
			const Uint32 steps = sprite > 0 ? (Uint32)((sprite - 1) / perStep) : 0;
			sprites.push_back({ link.centre + offset, firstFrame, link.at + steps * FireballFrameTime, 0 });
		}
	}
	// Counted in the order they start, so a view knows the ones new since its last frame.
	std::stable_sort(sprites.begin(), sprites.end(), [](const ChainSprite &a, const ChainSprite &b) { return a.start < b.start; });
	for (ChainSprite &sprite : sprites)
	{
		sprite.serial = ++chain.serials;
		chain.sprites.push_back(sprite);
	}

	// The tiles it changed (a part blown away or left as its wreckage, a fire started or put out):
	// held as they were until the fireball nearest to them (from the tile's middle) has grown over
	// them.
	const int tileCount = save->getMapSizeXYZ();
	chain.entries.assign(tileCount, -1);
	chain.held.clear();
	for (int index = 0; index < tileCount && index < (int)chain.before.size(); ++index)
	{
		Tile *tile = save->getTile(index);
		ChainHeldTile was = chain.before[index];
		bool changed = was.fire != tile->getFire();
		for (int part = 0; part < 4 && !changed; ++part)
		{
			changed = was.parts[part] != tile->getMapData((TilePart)part);
		}
		if (!changed)
		{
			continue;
		}
		const Position middle = tile->getPosition().toVoxel() + Position(8, 8, 12);
		int nearest = INT_MAX;
		Uint32 nearestAt = chain.links.front().at;
		for (const ChainLink &link : chain.links)
		{
			const int distance = Position::distanceSq(middle, link.centre);
			if (distance < nearest)
			{
				nearest = distance;
				nearestAt = link.at;
			}
		}
		was.revealAt = nearestAt + ChainRevealLag;
		chain.entries[index] = (int)chain.held.size();
		chain.held.push_back(was);
	}
	chain.before.clear();

	// The units it took down (dead or knocked out now): standing as they were until the ripple has
	// passed.
	chain.downed.clear();
	for (const ChainUnit &was : chain.standing)
	{
		if (was.unit->isOut())
		{
			chain.downed.push_back(was);
		}
	}
	chain.standing.clear();
}

}

/**
 * Called by the game as an explosive of a chain settling at once goes up (setting "Instant chain
 * explosions", see ExplosionBState::settleChain), before its blast changes the map: noted for the
 * ripple. The chain's first (a new chain) keeps the map and the units as they are now, after the
 * blast that set it off and before any of its own: every tile's parts and fire, and the units
 * still up (the blast's own dead are counted with the chain's), with their facing and whether the
 * player sees them.
 * @param save The battle.
 * @param centre Where it goes up (the game's blast centre), in voxels.
 * @param power Its power.
 * @param first Is it the chain's first?
 */
void chainLink(SavedBattleGame *save, const Position &centre, int power, bool first)
{
	ChainPicture &chain = chainPicture();
	const bool newBattle = chain.save != save;
	if (newBattle || (first && chain.settled))
	{
		if (newBattle)
		{
			chain.sprites.clear();
		}
		chain.save = save;
		chain.settled = false;
		chain.start = SDL_GetTicks();
		chain.links.clear();
		chain.entries.clear();
		chain.held.clear();
		chain.downed.clear();
		// Every tile as it is now.
		const int tileCount = save->getMapSizeXYZ();
		chain.before.resize(tileCount);
		for (int index = 0; index < tileCount; ++index)
		{
			Tile *tile = save->getTile(index);
			ChainHeldTile &was = chain.before[index];
			for (int part = 0; part < 4; ++part)
			{
				was.parts[part] = tile->getMapData((TilePart)part);
			}
			was.fire = tile->getFire();
			was.revealAt = 0;
		}
		// The units still up.
		chain.standing.clear();
		for (BattleUnit *unit : *save->getUnits())
		{
			if (!unit->isOut())
			{
				chain.standing.push_back({ unit, unit->getDirection(), unit->getVisible() || save->getDebugMode() });
			}
		}
	}
	chain.links.push_back({ centre, power, 0 });
}

/**
 * Called by the game as a chain settling at once is over (its last explosive set off no other):
 * its picture is worked out (see settleChain) before the 3D view's next frame.
 * @param save The battle.
 */
void chainSettled(SavedBattleGame *save)
{
	ChainPicture &chain = chainPicture();
	if (chain.save == save && !chain.settled)
	{
		settleChain(chain, save);
	}
}

/**
 * Is a chain's ripple still passing (its dead not fallen yet)? The battle waits for it while the
 * 3D view shows it (see FirstPersonMode::chainRippleHolds).
 * @param save The battle.
 * @return True until the last explosive's fireball fades.
 */
bool chainRippleShowing(const SavedBattleGame *save)
{
	const ChainPicture &chain = chainPicture();
	return chain.save == save && chain.settled && !chain.links.empty() && SDL_GetTicks() < chain.end;
}

/**
 * The fireballs of a chain's ripple showing now (each sprite from its start to its blast's last
 * frame), in the order they started; those whose frames are over are let go.
 * @param save The battle.
 * @param fireballs Set to them.
 */
void chainFireballs(const SavedBattleGame *save, std::vector<ChainFireball> &fireballs)
{
	fireballs.clear();
	ChainPicture &chain = chainPicture();
	if (chain.save != save || chain.sprites.empty())
	{
		return;
	}
	const Uint32 now = SDL_GetTicks();
	const Uint32 lasting = FireballFrameTime * Explosion::EXPLODE_FRAMES;
	chain.sprites.erase(std::remove_if(chain.sprites.begin(), chain.sprites.end(),
		[&](const ChainSprite &sprite) { return now >= sprite.start + lasting; }), chain.sprites.end());
	for (const ChainSprite &sprite : chain.sprites)
	{
		if (now < sprite.start)
		{
			continue;
		}
		const Uint32 age = now - sprite.start;
		ChainFireball fireball;
		fireball.position = sprite.position;
		fireball.frame = sprite.firstFrame + (int)(age / FireballFrameTime);
		fireball.lastFrame = sprite.firstFrame + Explosion::EXPLODE_FRAMES - 1;
		fireball.blend = (age % FireballFrameTime) / (float)FireballFrameTime;
		fireball.serial = sprite.serial;
		fireballs.push_back(fireball);
	}
}

/**
 * The tiles a chain's ripple hasn't passed over yet, as they were: for a view to take once a frame.
 * @param save The battle.
 * @return The tiles held (none without a ripple passing).
 */
ChainHeldTiles chainHeldTiles(const SavedBattleGame *save)
{
	const ChainPicture &chain = chainPicture();
	const Uint32 now = SDL_GetTicks();
	if (chain.save != save || !chain.settled || chain.held.empty() || now >= chain.end)
	{
		return ChainHeldTiles();
	}
	return ChainHeldTiles(&chain.entries, &chain.held, now);
}

/**
 * The facing a unit a chain took down had before it, to be drawn standing so until the ripple has
 * passed (the dead fall after it, as the battle goes on); only one the player saw then.
 * @param save The battle.
 * @param unit The unit.
 * @return Its facing then, or -1 for any other unit (or once the ripple has passed).
 */
int chainHeldStanding(const SavedBattleGame *save, const BattleUnit *unit)
{
	const ChainPicture &chain = chainPicture();
	if (chain.save != save || !chain.settled || chain.downed.empty() || SDL_GetTicks() >= chain.end)
	{
		return -1;
	}
	for (const ChainUnit &was : chain.downed)
	{
		if (was.unit == unit)
		{
			return was.seen ? was.direction : -1;
		}
	}
	return -1;
}

/**
 * Forgets any chain: a battle starting, or one loaded (its map and units are new).
 */
void clearChains()
{
	chainPicture() = ChainPicture();
}

/**
 * Stands a unit a chain took down up again for drawing, as it was before the chain: standing,
 * facing as it did (the game turned it to face south-east as it went down).
 * @param unit The unit (null: nothing).
 * @param direction Its facing before the chain.
 */
ChainStandingPose::ChainStandingPose(BattleUnit *unit, int direction) : _unit(unit), _gameDirection(unit ? unit->getDirection() : 0)
{
	if (!_unit)
	{
		return;
	}
	_unit->abortTurn(); // (standing)
	_unit->setDirection(direction);
}

/**
 * Puts the unit back as the battle has it: its facing, and down at the end of its fall (dead, or
 * knocked out with health left), as the game left it (BattleUnit::instaFalling).
 */
ChainStandingPose::~ChainStandingPose()
{
	if (_unit)
	{
		_unit->setDirection(_gameDirection);
		_unit->instaFalling();
	}
}

}

}
