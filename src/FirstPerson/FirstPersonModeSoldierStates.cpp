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
 * FirstPersonModeSoldierStates.cpp: what happens to the soldier we look through.
 *
 * Getting hurt (a red flash, the hit's direction, a heartbeat), shots whizzing past, catching
 * fire, going down (killed, unconscious, mind controlled, turned into a zombie) and breaking
 * down (panic, berserk), each shown its own way.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "../Battlescape/BattleState.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Projectile.h"
#include "../Engine/Game.h"
#include "../Interface/Text.h"
#include "../Mod/Armor.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

namespace
{

/**
 * Holds the battle for a moment while the 3D view shows a soldier's breakdown (see
 * FirstPersonMode::breakdown). The game runs its queue of battle states (walking, shooting,
 * panicking...) front first, so with this state put at the front the battle waits, as it would
 * behind the game's own "has panicked" message box; the 3D view leaves that box out, since it
 * would freeze the view behind it. After BreakdownPauseTime ms the state removes itself and the
 * battle goes on.
 */
class BreakdownPause : public BattleState
{
	Uint32 _endTick; // SDL ticks (ms) when the pause is over.
public:
	/// Starts the pause now, in the battle run by `parent`.
	BreakdownPause(BattlescapeGame *parent) : BattleState(parent), _endTick(SDL_GetTicks() + BreakdownPauseTime) { }
	/// Called by the battle every tick: once the time is up, takes this state off the queue.
	void think() override
	{
		if (SDL_GetTicks() >= _endTick) _parent->popState();
	}
};

}

/**
 * An enemy shot whizzed past close by (the 3D view notices it, see
 * FirstPersonView::getNearMissTick): hear it go past on its side, and flinch. Called every frame;
 * reacts once to each near miss.
 */
void FirstPersonMode::checkNearMiss()
{
	// Only a near miss we haven't reacted to yet.
	const Uint32 nearMissTick = _view->getNearMissTick();
	if (nearMissTick == _nearMissSeen)
	{
		return;
	}
	_nearMissSeen = nearMissTick;

	// Which way it passed, from the eye (on the map's x and y), as an angle from where the view
	// looks: 0 ahead, 90 right, in 0..360 degrees (the 720 keeps it positive for the modulo).
	double passDirection[2];
	_view->getNearMissDirection(passDirection);
	const double angle = std::fmod((std::atan2(passDirection[1], passDirection[0]) - _view->getYawDegrees() * Pi / 180.0) * 180.0 / Pi + 720.0, 360.0);

	// The whizz, panned to that side, and a small flinch of the camera.
	_sounds->play(FirstPersonSounds::WHIZZ, (int)angle, 0.85f);
	_view->addShake(0.8f);
}

/**
 * Shows what getting hurt feels like for the soldier we look through. A hit flashes the view red
 * (more for a bigger wound), shakes it, and shows an arc on the screen's edge towards where the
 * hit came from; a hit in melee, or a heavy one, also splatters blood over the view. While badly
 * hurt (under half health) the edges of the view pulse red with a heartbeat you hear. Called
 * every frame.
 * @param unit The soldier we look through.
 * @param now SDL ticks, in ms.
 */
void FirstPersonMode::checkHurt(BattleUnit *unit, Uint32 now)
{
	// A soldier we weren't watching: just note their health, to notice the next loss.
	if (unit != _healthWatchedUnit)
	{
		_healthWatchedUnit = unit;
		_lastHealth = unit->getHealth();
		return;
	}

	// Health went down since the last frame: a hit.
	const int health = unit->getHealth();
	if (health < _lastHealth)
	{
		// How hard it shows: from 0.3 up, a twelfth more per point lost, at most 0.85.
		_hurtStart = now;
		_hurtAmount = std::clamp((_lastHealth - health) / 12.0f + 0.3f, 0.3f, 0.85f);
		_view->addShake(1.0f + _hurtAmount * 2.0f);

		// Where it came from: the enemy shot that ended less than 0.8 seconds ago, or else an
		// alien striking us in melee (a hard jolt and blood over the view): one right beside us
		// on the same level (any tile of a big one's footprint) that is aiming, which the game
		// has an attacker do from the blow until its hit has played out (MeleeAttackBState,
		// ExplosionBState). Anything else (a fall, fire, bleeding) has no direction. The point
		// is in voxels: the shot's origin, or the middle of the alien's tile at half a level's
		// height.
		double hurtFrom[3];
		const Uint32 enemyShotEnd = _view->getEnemyShotEnd(hurtFrom);
		const bool fromShot = enemyShotEnd && now - enemyShotEnd < 800;
		const BattleUnit *attacker = nullptr;
		if (!fromShot)
		{
			const Position unitTile = unit->getPosition();
			for (const BattleUnit *other : *_save->getUnits())
			{
				// The footprint's nearest tile to ours, on each axis (a big unit's position is
				// its top-left tile).
				const Position otherTile = other->getPosition();
				const int footprint = other->getArmor()->getSize();
				const int nearestX = std::clamp<int>(unitTile.x, otherTile.x, otherTile.x + footprint - 1);
				const int nearestY = std::clamp<int>(unitTile.y, otherTile.y, otherTile.y + footprint - 1);
				if (!other->isOut() && other->getFaction() == FACTION_HOSTILE && other->getStatus() == STATUS_AIMING
					&& otherTile.z == unitTile.z && std::abs(nearestX - unitTile.x) <= 1 && std::abs(nearestY - unitTile.y) <= 1)
				{
					attacker = other;
				}
			}
			if (attacker)
			{
				const Position attackerTile = attacker->getPosition();
				hurtFrom[0] = attackerTile.x * 16.0 + 8.0; hurtFrom[1] = attackerTile.y * 16.0 + 8.0; hurtFrom[2] = attackerTile.z * 24.0 + 12.0;
			}
		}

		// The arc towards it (none if we don't know where it came from), with blood for melee or
		// a heavy hit; melee also knocks the view aside, left or right (by the time), and up.
		_view->showHurt(fromShot || attacker ? hurtFrom : nullptr, _hurtAmount, attacker || _hurtAmount > 0.55f);
		if (attacker)
		{
			_view->addShake(2.5f);
			_view->rotate((now / 97) % 2 ? 5.0 : -5.0, 3.0); // Knocked aside.
		}
	}
	_lastHealth = health;

	// Badly hurt: the edges of the view pulse red like a heartbeat. The danger grows from 0 at
	// half health to 1 at none; the pulse is a sine at 7.5 radians a second (about 72 beats a
	// minute), sharpened into a short throb.
	const int maxHealth = std::max(1, (int)unit->getBaseStats()->health);
	const float danger = std::clamp((0.5f - (float)health / maxHealth) / 0.5f, 0.0f, 1.0f);
	const float beat = std::pow(std::max(0.0f, std::sin(now / 1000.0f * 7.5f)), 4.0f);
	_view->setVignette(danger > 0.0f ? danger * (0.45f + 0.55f * beat) : 0.0f);

	// And you hear your heart, in time with the pulse: a thump at the start of each period of the
	// sine, louder the worse it is (none on the first frame, when there's no count to compare).
	const int heartbeatCount = (int)(now / 1000.0f * 7.5f / (2.0f * 3.14159265f));
	if (danger > 0.0f && heartbeatCount != _lastHeartbeat && _lastHeartbeat >= 0)
	{
		_sounds->play(FirstPersonSounds::HEARTBEAT, 0, 0.35f + 0.65f * danger);
	}
	_lastHeartbeat = heartbeatCount;

	// The red flash of the last hit fades away over 450 ms. Not while the next soldier's view is
	// still coming in after a fall: that fade colours the view itself (see deathCam).
	const Uint32 sinceHurt = now - _hurtStart;
	if (!_afterFallStart)
	{
		_view->setTint(_hurtStart && sinceHurt < 450 ? _hurtAmount * (1.0f - sinceHurt / 450.0f) : 0.0f);
	}
}

/**
 * Shows the soldier we look through being on fire: flames lick up from the bottom of the view,
 * fiercer the more turns the soldier will still burn. Catching fire (or burning longer) makes them
 * leap up, with a whoosh, a jolt and a BURNING banner for 1.6 seconds (see updateBanner).
 * Called every frame.
 * @param unit The soldier we look through.
 * @param now SDL ticks, in ms.
 */
void FirstPersonMode::checkBurning(BattleUnit *unit, Uint32 now)
{
	// How many turns the soldier will still burn; a soldier we weren't watching starts the count.
	const int fireTurns = unit->getFire();
	if (unit != _burningWatchedUnit)
	{
		_burningWatchedUnit = unit;
		_lastFireTurns = fireTurns;
	}

	// More turns than before: caught fire (again).
	if (fireTurns > _lastFireTurns)
	{
		_view->flareUp(1.0f);
		_view->addShake(1.5f);
		_sounds->play(FirstPersonSounds::FLARE, 0, 1.0f);
		_burningBannerUntil = now + 1600;
	}
	_lastFireTurns = fireTurns;

	// The flames: from 0.55 for one turn left, 0.1 more per turn, at most 0.75.
	_view->setBurning(fireTurns > 0 ? std::min(0.75f, 0.45f + 0.1f * fireTurns) : 0.0f);
}

/**
 * The soldier we were looking through has gone down, or was taken over: for a moment the view
 * stays with them, each way its own. Killed: they fall hard, the view washes red and the helmet
 * camera dies in static. Turned into a zombie: killed, then up again, lurching in a sickly green
 * until the camera dies. Unconscious: a slow sag into dark blue. Mind controlled: the view drifts,
 * washing orange. A banner says what happened, and whose camera it was. Then on to the next
 * soldier, whose view comes in out of the static or the colour. (With nobody left, the game ends
 * the battle, as on the map.)
 * Called every frame; also does that coming in, and the coming to after a breakdown
 * (see breakdownCam).
 * @param now SDL ticks, in ms.
 * @param elapsedSeconds Seconds since the last frame.
 * @return True while the fall is showing (the frame's other updates are skipped meanwhile).
 */
bool FirstPersonMode::deathCam(Uint32 now, double elapsedSeconds)
{
	// Has the soldier we follow just gone down (killed or unconscious), or been turned against us
	// (one of ours, now on another side: mind control)? Noticed once, and only in the 3D view.
	BattleUnit *unit = _trackedUnit;
	const bool wentDown = unit && unit->isOut();
	const bool takenOver = unit && !wentDown && unit->getOriginalFaction() == FACTION_PLAYER && unit->getFaction() != FACTION_PLAYER;
	if (!_deathUnit && (wentDown || takenOver) && isActive())
	{
		// What happened (a soldier the game has already converted into another unit has been
		// turned into a zombie), and the eye height they fell from (a zombie gets up to about it
		// again).
		_deathUnit = unit;
		_deathStart = now;
		_fallKind = takenOver ? FALL_CONTROLLED : unit->getAlreadyRespawned() ? FALL_ZOMBIE
			: unit->getStatus() == STATUS_DEAD ? FALL_KILLED : FALL_UNCONSCIOUS;
		_deathEyeZ = _eyeZ;

		// Whatever was going on stops: the flames die down, no step, no lean, no fade from an
		// earlier fall, no breakdown (they went down in the middle of one), and the crosshair, its
		// texts and the weapon in hand go (the weapon falls from the hand).
		_view->setBurning(0.0f);
		endLean();
		_stepPending = false;
		_guidedStep = false;
		_afterFallStart = 0;
		_breakdownUnit = nullptr;
		_breakdownStart = 0;
		_txtMode->setVisible(false);
		_txtShot->setVisible(false);
		_txtTarget->setVisible(false);
		_crosshair->setVisible(false);
		_weaponInHand->setVisible(false);

		// Was that the last soldier standing?
		bool anyoneLeft = false;
		for (const BattleUnit *other : *_save->getUnits())
		{
			if (other != unit && other->getFaction() == FACTION_PLAYER && !other->isOut()) anyoneLeft = true;
		}
		_allSoldiersLost = !anyoneLeft;

		// The banner: what happened, and under it whose camera this is (or ALL SOLDIERS LOST).
		const char *title = _fallKind == FALL_KILLED ? "STR_FPS_KILLED" : _fallKind == FALL_ZOMBIE ? "STR_FPS_ZOMBIFIED"
			: _fallKind == FALL_UNCONSCIOUS ? "STR_FPS_UNCONSCIOUS" : "STR_FPS_MIND_CONTROLLED";
		showFallBanner(_fallKind, _state->tr(title), anyoneLeft ? unit->getName(_game->getLanguage()) : std::string(_state->tr("STR_FPS_ALL_LOST")));
	}

	// No fall showing: if the next soldier's view is still coming in, fade out the static (after
	// a death) evenly, or the colour (dark blue after fainting, orange after mind control, white
	// after a breakdown, with some red after going berserk) slowly at first and then faster.
	if (!_deathUnit)
	{
		if (_afterFallStart)
		{
			const float fadeProgress = (now - _afterFallStart) / (float)std::max<Uint32>(1, _afterFallTime);
			if (fadeProgress >= 1.0f)
			{
				_afterFallStart = 0;
				_view->setStatic(0.0f);
				_view->setTint(0.0f);
			}
			else if (_afterFallKind == FALL_KILLED || _afterFallKind == FALL_ZOMBIE)
			{
				_view->setStatic(1.0f - fadeProgress);
			}
			else
			{
				const int tintColor = _afterFallKind == FALL_UNCONSCIOUS ? 1 : _afterFallKind == FALL_CONTROLLED ? 2 : 3;
				_view->setTint(1.0f - fadeProgress * fadeProgress, tintColor, _afterFallKind == FALL_BERSERK ? 0.3f * (1.0f - fadeProgress) : 0.0f);
			}
		}
		return false;
	}

	// Killed, then turned into a zombie: the game turns them a moment after they fall, so switch
	// to the zombie's show if that comes within 1.2 seconds.
	if (_fallKind == FALL_KILLED && _deathUnit->getAlreadyRespawned() && now - _deathStart < 1200)
	{
		_fallKind = FALL_ZOMBIE;
		showFallBanner(FALL_ZOMBIE, _state->tr("STR_FPS_ZOMBIFIED"),
			_allSoldiersLost ? std::string(_state->tr("STR_FPS_ALL_LOST")) : _deathUnit->getName(_game->getLanguage()));
	}

	// How long this kind of fall shows. Once it's over, hand over to the next soldier: their view
	// comes in out of this one (see above), the banner goes, and the camera is placed afresh on
	// whoever is selected next.
	const Uint32 showTime = _fallKind == FALL_KILLED ? DeathCamTime : _fallKind == FALL_ZOMBIE ? ZombieCamTime
		: _fallKind == FALL_UNCONSCIOUS ? FaintTime : ControlledTime;
	const Uint32 sinceFall = now - _deathStart;
	if (sinceFall >= showTime)
	{
		_afterFallStart = now;
		_afterFallKind = _fallKind;
		_afterFallTime = AfterFallTime;
		_deathUnit = nullptr;
		_trackedUnit = nullptr;
		_view->setVignette(0.0f);
		_txtBanner->setVisible(false);
		for (Text *glow : _bannerGlow) glow->setVisible(false);
		_txtFallName->setVisible(false);
		syncShadows();

		// Nobody left to play: the only thing left is the end of the turn, where the game ends
		// the battle (asked without the usual confirmation).
		if (_allSoldiersLost && _save->getSide() == FACTION_PLAYER && !_state->_battleGame->isBusy())
		{
			_state->_battleGame->requestEndTurn(false);
		}
		return false;
	}

	// How far through the show we are (0 to 1), and the eye height of someone lying on the floor
	// of their tile (5 voxels up; a level is 24).
	const float progress = sinceFall / (float)showTime;
	const double lyingEyeZ = _deathUnit->getPosition().z * 24.0 + 5.0;
	switch (_fallKind)
	{
	case FALL_KILLED:
		// Down hard, keeling over, washing red; then, past the half, the camera's signal goes.
		_eyeZ += (lyingEyeZ - _eyeZ) * std::min(1.0, elapsedSeconds * 5.0);
		_view->rotate(elapsedSeconds * 25.0, elapsedSeconds * 30.0 * std::max(0.0f, 1.0f - 2.0f * progress));
		_view->setTint(std::min(1.0f, 0.45f + progress * 1.6f));
		_view->setStatic(std::clamp((progress - 0.45f) / 0.4f, 0.0f, 1.0f));
		if (progress > 0.5f) _txtFallName->setText(std::string(_deathUnit->getName(_game->getLanguage())) + " - " + std::string(_state->tr("STR_FPS_SIGNAL_LOST")));
		break;
	case FALL_UNCONSCIOUS:
		// A slow sag, the head dropping, into dark blue.
		_eyeZ += (lyingEyeZ - _eyeZ) * std::min(1.0, elapsedSeconds * 1.6);
		_view->rotate(elapsedSeconds * 8.0, -elapsedSeconds * 14.0 * (1.0 - progress));
		_view->setTint(std::min(1.0f, 0.25f + progress * 1.2f), 1);
		break;
	case FALL_CONTROLLED:
		// The soldier's own mind slips: the view drifts and sways, washing orange (with a flicker).
		_view->rotate(std::sin(sinceFall * 0.004) * elapsedSeconds * 40.0, std::cos(sinceFall * 0.003) * elapsedSeconds * 15.0);
		_view->setTint(std::min(1.0f, 0.2f + progress * 1.1f + 0.08f * (float)std::sin(sinceFall * 0.02)), 2);
		break;
	case FALL_ZOMBIE:
	{
		// Down hard and red, a moment still... then the body gets up again, hunched and lurching,
		// the red turning a sickly green, until the helmet camera gives out. The rise runs from 38%
		// to 72% of the show, the static from 72% to 97%.
		const float rise = std::clamp((progress - 0.38f) / 0.34f, 0.0f, 1.0f);
		if (rise <= 0.0f)
		{
			// Falling: as when killed, the turn and tilt dying away within the first quarter.
			_eyeZ += (lyingEyeZ - _eyeZ) * std::min(1.0, elapsedSeconds * 5.0);
			_view->rotate(elapsedSeconds * 25.0 * std::max(0.0f, 1.0f - 4.0f * progress), elapsedSeconds * 30.0 * std::max(0.0f, 1.0f - 4.0f * progress));
			_view->setTint(std::min(0.85f, 0.45f + progress * 2.0f));
		}
		else
		{
			// Rising, eased in and out, to 90% of the old eye height; swaying, with the head
			// hanging (the view pitched about 12 degrees down); the red share of the wash giving
			// way to green.
			const double risingEyeZ = lyingEyeZ + (_deathEyeZ - lyingEyeZ) * 0.9 * rise * rise * (3.0 - 2.0 * rise);
			_eyeZ += (risingEyeZ - _eyeZ) * std::min(1.0, elapsedSeconds * 4.0);
			const double droop = -12.0 - _view->getPitchDegrees(); // The head hangs.
			_view->rotate(std::sin(sinceFall * 0.0026) * elapsedSeconds * 35.0,
				droop * std::min(1.0, elapsedSeconds * 2.5) + std::sin(sinceFall * 0.0041) * elapsedSeconds * 10.0);
			_view->setTint(0.85f, 4, 1.0f - rise);
		}
		_view->setStatic(std::clamp((progress - 0.72f) / 0.25f, 0.0f, 1.0f));
		if (progress > 0.8f) _txtFallName->setText(std::string(_deathUnit->getName(_game->getLanguage())) + " - " + std::string(_state->tr("STR_FPS_SIGNAL_LOST")));
		break;
	}
	default:
		break;
	}

	// Put the eye where the fall left it, and have the view redrawn.
	_view->setEye(_eyeX, _eyeY, _eyeZ);
	_view->invalidate();
	return true;
}

/**
 * Puts up the banner saying what happened to the soldier we're looking through, in that fall's
 * colours (a bright core in a halo: the banner's glow copies and its shadow), and the line under
 * it (whose camera this is).
 * @param kind What happened; it picks the colours.
 * @param title The banner's text, already translated.
 * @param name The line under it: the soldier's name, or another text.
 */
void FirstPersonMode::showFallBanner(FallKind kind, const std::string &title, const std::string &name)
{
	// The colours for each kind, from the battlescape palette. Texts normally take a colour
	// group's start, their glyph shades running down the group (textColor); the dark letters on
	// the white of a panic take plain contrast and a shade of their own (inkColor).
	const SDL_Color *palette = _state->getPalette();
	Uint8 core = 0, halo = 0;
	const bool darkLetters = kind == FALL_PANIC;
	switch (kind)
	{
	case FALL_KILLED: core = textColor(palette, 255, 70, 50); halo = textColor(palette, 150, 10, 10); break;
	case FALL_UNCONSCIOUS: core = textColor(palette, 140, 180, 255); halo = textColor(palette, 30, 40, 140); break;
	case FALL_CONTROLLED: core = textColor(palette, 255, 170, 60); halo = textColor(palette, 160, 70, 0); break;
	case FALL_PANIC: core = inkColor(palette, 14, 14, 18); halo = inkColor(palette, 95, 95, 100); break;
	case FALL_BERSERK: core = textColor(palette, 255, 40, 30); halo = inkColor(palette, 70, 0, 0); break;
	case FALL_ZOMBIE: core = textColor(palette, 150, 230, 90); halo = textColor(palette, 30, 90, 20); break;
	}

	// The banner in the core colour, its glow copies around it in the halo colour.
	_txtBanner->setHighContrast(!darkLetters);
	_txtFallName->setHighContrast(!darkLetters);
	_txtBanner->setText(title);
	_txtBanner->setColor(core);
	for (Text *glow : _bannerGlow)
	{
		glow->setText(title);
		glow->setColor(halo);
		glow->setVisible(true);
	}
	_txtBanner->setVisible(true);
	_textShadows.front()->setColor(halo); // The banner's shadow is part of the halo.

	// The line under it, then the shadows follow the texts.
	_txtFallName->setText(name);
	_txtFallName->setColor(core);
	_txtFallName->setVisible(true);
	syncShadows();
}

/**
 * A soldier panics or goes berserk at the start of the turn: the game asks us before it shows
 * its "has panicked" box (see FirstPerson::breakdown). In 3D the view shows it instead, through
 * the soldier's eyes (see breakdownCam), while the game holds a moment (see BreakdownPause) and
 * the soldier then does what they do (freeze, flee or fire wildly). Either way, their camera
 * says so for the rest of the turn (see _breakdownTurns).
 * @param state The battlescape screen (its first-person mode, if any, shows it).
 * @param unit The soldier breaking down; their status says whether they panic or go berserk.
 * @return True if the 3D view shows it (the game then leaves its box out).
 */
bool FirstPersonMode::breakdown(BattlescapeState *state, BattleUnit *unit)
{
	// Only for our own soldiers on our turn, with a first-person mode to show it.
	FirstPersonMode *firstPerson = state ? state->_firstPerson : nullptr;
	if (!firstPerson || !unit || unit->getFaction() != FACTION_PLAYER || firstPerson->_save->getSide() != FACTION_PLAYER)
	{
		return false;
	}

	// Remember who broke down in which turn, and how (berserk or panic), for their camera.
	const bool berserk = unit->getStatus() == STATUS_BERSERK;
	firstPerson->_breakdownTurns[unit] = { firstPerson->_save->getTurn(), berserk };

	// On the isometric map, the game shows its box as usual.
	if (!firstPerson->_wants3D)
	{
		return false;
	}

	// In 3D: the view shows it from the next frame (one soldier at a time: the next one's
	// breakdown starts afresh), and the battle waits a moment at the front of the game's queue.
	firstPerson->_breakdownUnit = unit;
	firstPerson->_breakdownBerserk = berserk;
	firstPerson->_breakdownStart = 0;
	state->_battleGame->statePushFront(new BreakdownPause(state->_battleGame));
	return true;
}

/**
 * A soldier's breakdown, through their eyes: the view trembles and washes out white, the heart
 * racing and thumping; gone berserk, red throbs through the white with each beat, and every shot
 * they fire flashes red. It stays while they freeze, flee or fire wildly, at least
 * BreakdownMinTime ms, and then they come to (the white fades away, see deathCam). Called every
 * frame.
 * @param now SDL ticks, in ms.
 * @param elapsedSeconds Seconds since the last frame.
 * @return True while the breakdown is showing (the frame's other updates are skipped meanwhile).
 */
bool FirstPersonMode::breakdownCam(Uint32 now, double elapsedSeconds)
{
	// Nothing to show; or back on the isometric map (kept, to show again on coming back to 3D);
	// or the soldier went down or was taken over meanwhile (the death camera has that), or the
	// turn is over.
	BattleUnit *unit = _breakdownUnit;
	if (!unit)
	{
		return false;
	}
	if (!_wants3D)
	{
		endBreakdown(false);
		return false;
	}
	if (unit->isOut() || unit->getFaction() != FACTION_PLAYER || _save->getSide() != FACTION_PLAYER)
	{
		endBreakdown();
		return false;
	}

	// The first frame: start the show. Note whether a shot is already in the air (only new ones
	// flash), start the heartbeat count afresh, end any fade from an earlier fall, switch to the
	// 3D view if needed, clear the static and the red edges, hide the crosshair, its texts and the
	// weapon in hand, and put up the banner.
	if (!_breakdownStart)
	{
		_breakdownStart = now;
		_breakdownShot = _state->_map->getProjectile() != nullptr;
		_breakdownFlash = 0;
		_lastHeartbeat = -1;
		_afterFallStart = 0;
		if (!isActive())
		{
			setActive(true);
		}
		_view->setStatic(0.0f);
		_view->setVignette(0.0f);
		_txtMode->setVisible(false);
		_txtShot->setVisible(false);
		_txtTarget->setVisible(false);
		_crosshair->setVisible(false);
		_weaponInHand->setVisible(false);
		const FallKind kind = _breakdownBerserk ? FALL_BERSERK : FALL_PANIC;
		showFallBanner(kind, _state->tr(_breakdownBerserk ? "STR_FPS_BERSERK" : "STR_FPS_BREAKDOWN"), unit->getName(_game->getLanguage()));
	}

	// Over once the game is done with them (nothing going on, and the panic handled or another
	// soldier selected; the next soldier's breakdown, if any, starts afresh) and it has shown
	// long enough. They come to: the white (and red) fades away over ComeToTime ms (see deathCam).
	const Uint32 sinceStart = now - _breakdownStart;
	const bool gameDone = !_state->_battleGame->isBusy() && (_state->_battleGame->getPanicHandled() || _save->getSelectedUnit() != unit);
	if (gameDone && sinceStart >= BreakdownMinTime)
	{
		const FallKind kind = _breakdownBerserk ? FALL_BERSERK : FALL_PANIC;
		endBreakdown();
		_afterFallStart = now;
		_afterFallKind = kind;
		_afterFallTime = ComeToTime;
		_view->setTint(1.0f, 3, kind == FALL_BERSERK ? 0.3f : 0.0f);
		return false;
	}

	// Stay on them wherever they go (fleeing; the keys don't move them), the view turning where
	// they turn (to fire: quicker when berserk), trembling all the while. A unit's direction is
	// 0 for north in 45 degree steps, the view's yaw 0 for east: hence the 90 degrees.
	updateCamera(unit, false, elapsedSeconds);
	const double turnDegrees = std::remainder(unit->getDirection() * 45.0 - 90.0 - _view->getYawDegrees(), 360.0);
	_view->rotate(turnDegrees * std::min(1.0, elapsedSeconds * (_breakdownBerserk ? 6.0 : 2.0)) + std::sin(sinceStart * 0.0023) * elapsedSeconds * 14.0,
		std::sin(sinceStart * 0.0017) * elapsedSeconds * 9.0);
	_view->addShake(_breakdownBerserk ? 0.5f : 0.3f);

	// Keep the top bar, the flames, the info panel and the armor buttons up to date meanwhile.
	updateTopBar(_save->getSelectedUnit());
	_view->setBurning(unit->getFire() > 0 ? std::min(0.75f, 0.45f + 0.1f * unit->getFire()) : 0.0f);
	updateInfoPanel(unit);
	updateArmorButtons(unit);
	syncShadows();

	// The racing heart (2.6 beats a second berserk, 2.1 in a panic): each beat thumps and jolts
	// the view.
	const float beatCount = sinceStart / 1000.0f * (_breakdownBerserk ? 2.6f : 2.1f);
	if ((int)beatCount != _lastHeartbeat)
	{
		_lastHeartbeat = (int)beatCount;
		_sounds->play(FirstPersonSounds::HEARTBEAT, 0, 0.9f);
		_view->addShake(_breakdownBerserk ? 1.2f : 0.9f);
	}

	// The wash: each beat's throb dies away quickly after it; the white comes in over 1.5
	// seconds, eased, from 15% to full.
	const float throb = std::exp(-(beatCount - std::floor(beatCount)) * 7.0f);
	const float fadeIn = std::min(1.0f, sinceStart / 1500.0f);
	const float whiteAmount = 0.15f + 0.85f * fadeIn * fadeIn * (3.0f - 2.0f * fadeIn);

	// Berserk: a share of the washed pixels goes red, throbbing with the heart, and each new shot
	// in the air flashes red for 300 ms, with a jolt.
	float redShare = 0.0f;
	if (_breakdownBerserk)
	{
		const bool shotInAir = _state->_map->getProjectile() != nullptr;
		if (shotInAir && !_breakdownShot)
		{
			_breakdownFlash = now;
			_view->addShake(2.0f);
		}
		_breakdownShot = shotInAir;
		const float flash = _breakdownFlash && now - _breakdownFlash < 300 ? 1.0f - (now - _breakdownFlash) / 300.0f : 0.0f;
		redShare = std::min(0.9f, (0.2f + 0.5f * throb) * fadeIn + 0.7f * flash);
	}
	_view->setTint(whiteAmount, 3, redShare);
	_view->invalidate();
	return true;
}

/**
 * Ends a breakdown's show: takes its banner and white wash down (if it was showing yet) and
 * forgets the breakdown; or, not forgetting (on going back to the isometric map), keeps it to
 * show again from the start.
 * @param forget True to forget the breakdown, false to keep it for later.
 */
void FirstPersonMode::endBreakdown(bool forget)
{
	// It was showing: take the banner down, back to the usual high contrast letters, and clear
	// the wash.
	if (_breakdownUnit && _breakdownStart)
	{
		_txtBanner->setVisible(false);
		for (Text *glow : _bannerGlow) glow->setVisible(false);
		_txtFallName->setVisible(false);
		_txtBanner->setHighContrast(true);
		_txtFallName->setHighContrast(true);
		_view->setTint(0.0f);
		syncShadows();
	}

	// Forget it, or keep it to start over; either way it isn't showing now.
	if (forget)
	{
		_breakdownUnit = nullptr;
	}
	_breakdownStart = 0;
	_lastHeartbeat = -1;
}

/**
 * The aliens' turn in 3D (setting "Aliens' turn cameras", with "Alien turn in 3D"): instead of
 * staying on the last soldier, the view goes where the action is, and soldiers in trouble pop up
 * a small camera (the soldier card's helmet camera, see updateSoldierCard). In order:
 * - Watching the squad: a soldier of ours who is hit (health or stun), taken over (mind
 *   control) or goes down (setting "Camera when a soldier falls") pops up their camera for
 *   AlertTime; their status says why ("HIT", or by itself MIND CONTROLLED, or DECEASED or
 *   UNCONSCIOUS over the static of a dead camera). The soldier the view looks through going
 *   down is the death camera's (see deathCam).
 * - A shot in flight: one at a soldier of ours from an alien pops up that soldier's camera
 *   ("TARGETED"); one fired by a soldier of ours (reaction fire) cuts the view to the shooter at
 *   once, looking at what they shoot at.
 * - The alien acting now: when the soldier looked through doesn't see it, the view cuts to the
 *   nearest soldier who does, but never sooner than AlienCameraHold after the last cut (no
 *   flicking back and forth); and while looking through someone who sees it, the view glides
 *   round to the alien each time it reaches a new tile (and leaves the mouse alone otherwise).
 * When no soldier sees anything, the view stays where it is (HIDDEN MOVEMENT).
 * @param now The time (SDL ticks).
 * @return The soldier to look through; null for the last soldier (the setting off, or nobody yet).
 */
BattleUnit *FirstPersonMode::alienTurnCamera(Uint32 now)
{
	// A soldier of ours fit to look through.
	auto canLookThrough = [](const BattleUnit *unit) { return unit && unit->getFaction() == FACTION_PLAYER && !unit->isOut() && unit->getTile(); };
	if (!Options::fpsAlienTurnCameras)
	{
		_alienTurnCamera = nullptr;
		return nullptr;
	}
	if (!canLookThrough(_alienTurnCamera))
	{
		_alienTurnCamera = nullptr;
	}

	// The squad, watched from frame to frame: hit (less health, or more stun), taken over, or
	// gone down (dead or unconscious: side -1).
	for (BattleUnit *soldier : *_save->getUnits())
	{
		if (soldier->getOriginalFaction() != FACTION_PLAYER) continue;
		const int wellBeing = soldier->getHealth() - soldier->getStunlevel();
		const int side = soldier->isOut() ? -1 : (int)soldier->getFaction();
		const auto lastFrame = _alertWatch.find(soldier);
		if (lastFrame != _alertWatch.end())
		{
			const int sideBefore = lastFrame->second.second;
			if (side == -1 && sideBefore != -1) { if (Options::fpsFallenSoldierCamera) popUpCamera(soldier, "", now); }
			else if (side == FACTION_PLAYER && wellBeing < lastFrame->second.first) popUpCamera(soldier, "STR_FPS_ALERT_HIT", now);
			else if (side != -1 && side != FACTION_PLAYER && sideBefore == FACTION_PLAYER) popUpCamera(soldier, "", now);
		}
		_alertWatch[soldier] = { wellBeing, side };
	}

	// A shot in flight: who fired it (the unit on the tile it started from), and at whom.
	BattleUnit *shooter = nullptr, *shotAt = nullptr;
	if (const Projectile *projectile = _state->_map->getProjectile())
	{
		const Tile *fromTile = _save->getTile(projectile->getOrigin()), *toTile = _save->getTile(projectile->getTarget());
		shooter = fromTile ? fromTile->getUnit() : nullptr;
		shotAt = toTile ? toTile->getUnit() : nullptr;
		if (shooter && shooter->getFaction() != FACTION_PLAYER && canLookThrough(shotAt) && !(_alertUnit == shotAt && now < _alertUntil))
		{
			popUpCamera(shotAt, "STR_FPS_ALERT_TARGETED", now);
		}
	}

	// Reaction fire: one of ours shooting cuts the view to them at once.
	if (canLookThrough(shooter))
	{
		if (shooter != _alienTurnCamera) cutAlienTurnCamera(shooter, shotAt, now);
		return _alienTurnCamera;
	}

	// The alien acting now (the game selects each alien as it acts), and who of ours sees it.
	BattleUnit *acting = _save->getSelectedUnit();
	if (acting && acting->getFaction() != FACTION_PLAYER && !acting->isOut() && acting->getTile())
	{
		const bool watchedSees = _alienTurnCamera && _alienTurnCamera->hasVisibleUnit(acting);
		if (!watchedSees && now - _alienTurnCameraSince >= AlienCameraHold)
		{
			// The nearest soldier who sees it.
			BattleUnit *spotter = nullptr;
			int nearestSquared = INT_MAX;
			for (BattleUnit *soldier : *_save->getUnits())
			{
				if (!canLookThrough(soldier) || !soldier->hasVisibleUnit(acting)) continue;
				const int distanceSquared = soldier->distance3dToUnitSq(acting);
				if (distanceSquared < nearestSquared)
				{
					nearestSquared = distanceSquared;
					spotter = soldier;
				}
			}
			if (spotter) cutAlienTurnCamera(spotter, acting, now);
		}
		else if (watchedSees && (acting != _alienTurnWatched || acting->getPosition() != _alienTurnWatchedTile))
		{
			// Still in sight: follow it with the eyes when it moves on a tile.
			glideToLookAt(acting);
			_alienTurnWatched = acting;
			_alienTurnWatchedTile = acting->getPosition();
		}
	}
	return _alienTurnCamera;
}

/**
 * Cuts the aliens' turn view to a soldier, turning to look at a unit: at once if the view is
 * already theirs (gliding), else as the view switches to them (see updateCamera).
 * @param soldier The soldier to look through.
 * @param lookAt The unit to look at (null: the way the soldier faces).
 * @param now The time (SDL ticks).
 */
void FirstPersonMode::cutAlienTurnCamera(BattleUnit *soldier, const BattleUnit *lookAt, Uint32 now)
{
	_alienTurnCamera = soldier;
	_alienTurnCameraSince = now;
	_alienTurnWatched = lookAt;
	_alienTurnWatchedTile = lookAt ? lookAt->getPosition() : Position(-1, -1, -1);
	if (!lookAt)
	{
		return;
	}
	if (soldier == _trackedUnit) glideToLookAt(lookAt);
	else _lookAtOnSwitch = lookAt;
}

/**
 * Pops up the small camera (the soldier card, see updateSoldierCard) on a soldier for
 * AlertTime, unless it's the soldier the view already looks through.
 * @param soldier The soldier.
 * @param why A string id for their status line ("HIT"); empty: their own status.
 * @param now The time (SDL ticks).
 */
void FirstPersonMode::popUpCamera(const BattleUnit *soldier, const std::string &why, Uint32 now)
{
	if (soldier == _trackedUnit && isActive())
	{
		return;
	}
	_alertUnit = soldier;
	_alertText = why;
	_alertUntil = now + AlertTime;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
