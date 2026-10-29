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
 * FirstPersonQuickDeaths.cpp: setting "Quick deaths": a unit killed or knocked out is down at once
 * for the battle, and its fall plays on its own, in 3D and on the map, several at once.
 */
#include "FirstPersonApi.h"
#include <algorithm>
#include <vector>
#include <SDL.h>
#include "FirstPersonMode.h"
#include "FirstPersonViewShared.h"
#include "../Mod/Armor.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
namespace FirstPerson
{

namespace
{

/**
 * A fall under way: the unit, the battle it's in, when it went down, and the tiles it lay on
 * (its north-west corner and its size: 1, or 2 for a big unit).
 */
struct QuickFall
{
	const SavedBattleGame *save;
	BattleUnit *unit;
	Uint32 start;
	Position corner;
	int size;
};

/**
 * The falls under way (a few at most: they last a fraction of a second).
 * @return The list.
 */
std::vector<QuickFall> &quickFalls()
{
	static std::vector<QuickFall> falls;
	return falls;
}

/**
 * Is a fall surely over by now, without looking at its unit (it may be gone with its battle)?
 * No armor has more than QuickFallMaxFrames frames of falling.
 * @param fall The fall.
 * @param now The time (SDL ticks).
 * @return True if it's over.
 */
bool surelyOver(const QuickFall &fall, Uint32 now)
{
	return now - fall.start >= (Uint32)QuickFallMaxFrames * QuickFallFrameTime;
}

}

/**
 * Setting "Quick deaths" (a player's report, GitHub issue #40: deaths that don't hold up an auto
 * fire, several at once, "like doomguy chaingunning down zombies"): on, and the mod's 3D view in
 * the battle (the battle's rule, in 3D and on the map).
 * @param state The battle's screen.
 * @return True if deaths are quick.
 */
bool quickDeaths(BattlescapeState *state)
{
	return FirstPersonMode::quickDeaths(state);
}

/**
 * Notes a unit going down quickly (setting "Quick deaths"): it's down at once for the battle,
 * and its fall plays from now on (see quickFallPhase). Called as the game takes it out, before
 * it leaves its tile (UnitDieBState).
 * @param save The battle.
 * @param unit The unit.
 */
void noteQuickFall(const SavedBattleGame *save, BattleUnit *unit)
{
	const Uint32 now = SDL_GetTicks();
	std::vector<QuickFall> &falls = quickFalls();
	falls.erase(std::remove_if(falls.begin(), falls.end(), [&](const QuickFall &fall) { return surelyOver(fall, now) || fall.unit == unit; }), falls.end());
	falls.push_back({ save, unit, now, unit->getPosition(), unit->getArmor()->getSize() });
}

/**
 * Forgets the falls under way (setting "Quick deaths"): a battle is starting (or loaded), and its
 * units are new ones, which may even sit where the last battle's were in memory. Called as the
 * battle's 3D view is created (FirstPersonMode).
 */
void clearQuickFalls()
{
	quickFalls().clear();
}

/**
 * How far a unit's quick fall is (setting "Quick deaths"): the frame of its fall to draw, the
 * game's own fall frames at twice its pace (QuickFallFrameTime each), with no turn to face the
 * screen first.
 * @param unit The unit.
 * @return The frame (0 to its armor's death frames less one), or -1 if it isn't falling (never
 *   went down quickly, the fall is over, or it's no longer down: woken, or turned into another
 *   unit, a zombie, which stands there instead). It falls from the moment it's down, still on its
 *   tile for a moment (the game draws a unit down there as nothing) and after it has left it.
 */
int quickFallPhase(const BattleUnit *unit)
{
	const Uint32 now = SDL_GetTicks();
	for (const QuickFall &fall : quickFalls())
	{
		if (fall.unit != unit || surelyOver(fall, now))
		{
			continue;
		}
		const int frame = (int)((now - fall.start) / QuickFallFrameTime);
		const bool down = (unit->getStatus() == STATUS_DEAD || unit->getStatus() == STATUS_UNCONSCIOUS) && !unit->getAlreadyRespawned();
		return down && frame < std::max(1, unit->getArmor()->getDeathFrames()) ? frame : -1;
	}
	return -1;
}

/**
 * The unit whose quick fall is playing on this tile (setting "Quick deaths"): the map and the 3D
 * view draw it there, falling, and keep the body and items on the tile back until it's down.
 * @param save The battle.
 * @param tile The tile.
 * @return The unit, or null.
 */
BattleUnit *quickFallOn(const SavedBattleGame *save, const Tile *tile)
{
	if (quickFalls().empty() || !tile)
	{
		return nullptr;
	}
	const Uint32 now = SDL_GetTicks();
	const Position position = tile->getPosition();
	for (const QuickFall &fall : quickFalls())
	{
		const bool onIt = position.z == fall.corner.z && position.x >= fall.corner.x && position.y >= fall.corner.y
			&& position.x < fall.corner.x + fall.size && position.y < fall.corner.y + fall.size;
		if (fall.save == save && onIt && !surelyOver(fall, now) && quickFallPhase(fall.unit) >= 0)
		{
			return fall.unit;
		}
	}
	return nullptr;
}

/**
 * Puts a unit in its quick fall's pose for drawing (the game draws a falling unit by its status
 * and fall frame, see UnitSprite): falling, at the frame quickFallPhase gives. Nothing for a unit
 * that isn't falling (or null).
 * @param unit The unit, or null.
 */
QuickFallPose::QuickFallPose(BattleUnit *unit) : _unit(nullptr)
{
	const int phase = unit ? quickFallPhase(unit) : -1;
	if (phase < 0)
	{
		return;
	}
	_unit = unit;
	_unit->startFalling();
	for (int step = 0; step < phase; ++step)
	{
		_unit->keepFalling();
	}
}

/**
 * Puts the unit back as the battle has it: down, at the end of its fall (dead, or unconscious
 * with health left), as the game left it (BattleUnit::instaFalling).
 */
QuickFallPose::~QuickFallPose()
{
	if (_unit)
	{
		_unit->instaFalling();
	}
}

}
}
