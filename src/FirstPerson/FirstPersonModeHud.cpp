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
 * FirstPersonModeHud.cpp: the heads-up display of the first-person mode.
 *
 * The top bar (Iso/3D, HUD, INV, the armor's buttons, one button per soldier, END), the info
 * panel (name, weapon, status, the four stats), the soldier card shown over a squad button
 * (with a live view from their helmet camera), and the banner across the top of the view.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include "FirstPersonApi.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Pathfinding.h"
#include "../Engine/Font.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Unicode.h"
#include "../Interface/Bar.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Soldier.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Tells whether this unit can use night vision in the 3D view.
 *
 * With the option "Night vision needs gear" off, everyone has it (as on the isometric map).
 * With it on, night vision comes with gear: an armor or an inventory item that the mod's
 * firstPerson rules list as giving night vision, or eyes that see as far in the dark as by day
 * (aliens, whose view distance is the same by night as by day).
 * @param unit The unit to ask about; null has no night vision.
 * @return True if the unit may switch night vision on.
 */
bool FirstPersonMode::hasNightVision(const BattleUnit *unit) const
{
	// No unit, no eyes.
	if (!unit)
	{
		return false;
	}
	// Gear not required, or the unit sees in the dark by nature.
	if (!Options::fpsNightVisionGear || unit->getMaxViewDistanceAtDark(nullptr) >= unit->getMaxViewDistanceAtDay(nullptr))
	{
		return true;
	}
	// An armor the mod lists as having night vision built in.
	const RuleFirstPerson &rules = _game->getMod()->getFirstPerson();
	if (rules.givesNightVision(unit->getArmor()->getType()))
	{
		return true;
	}
	// Or any item the soldier carries (night vision goggles and the like), wherever it is in the
	// inventory.
	for (const BattleItem *item : *unit->getInventory())
	{
		if (rules.givesNightVision(item->getRules()->getType()))
		{
			return true;
		}
	}
	return false;
}

/**
 * Switches night vision on or off: what the NIGHT VISION button on the top bar and its key do.
 * A soldier without the gear (see hasNightVision) gets a warning instead. The switch itself is
 * the isometric map's own (the one its night vision button flips), so the map and the 3D view
 * share it; but unlike that button it works whatever the game is doing (an explosion, a shot in
 * flight, the aliens' turn), as it only changes how the picture is drawn (a player's report:
 * night vision couldn't be switched while a chain of explosions went off).
 * @param unit The soldier the view looks through.
 */
void FirstPersonMode::toggleNightVision(BattleUnit *unit)
{
	// No gear: say so and leave night vision as it is.
	if (!hasNightVision(unit))
	{
		_state->warning("STR_FPS_NO_NIGHT_VISION");
		return;
	}
	_state->_map->toggleNightVision();
}

/**
 * The compass (setting "Compass"), top right: a disc that turns as you turn under a fixed arrow
 * on top of it, which shows your heading; the letters of the compass go round with the disc. A
 * yellow triangle on its rim shows the way the soldier faces. Turning by the rules (setting "Turning costs TU"), an arc shows the head's free turn,
 * HeadTurn degrees either side of the facing: within CompassWarnDegrees of its end it turns
 * yellow and the line under it says what looking further costs ("TURN 1 TU"), or red with
 * "NO TU" when the soldier couldn't pay for the turn. A shot or throw aimed off the eighth the
 * soldier faces makes the game turn the soldier first and charge the turn (turning by the rules,
 * see shotTurnCost): an orange triangle on the rim marks the way the soldier will face, and the
 * line under the dial says what the turn adds ("SHOT +2 TU"; red when the time units won't
 * cover the turn and the shot); a melee blow at a unit or a wall beside the soldier the same
 * ("HIT +2 TU"). The dial is drawn again only when what it shows changes.
 * @param unit The soldier the view looks through, or null to hide the compass.
 * @param playerTurn It's the player's turn (the shot's turn is shown only then).
 */
void FirstPersonMode::updateCompass(BattleUnit *unit, bool playerTurn)
{
	const bool show = Options::fpsCompass && isActive() && !_hudHidden && unit;
	_compass->setVisible(show);
	for (Text *letter : _compassLetters) letter->setVisible(show);
	updateClimbArrows(show && playerTurn ? unit : nullptr);
	if (!show)
	{
		_txtCompassTurn->setVisible(false);
		return;
	}

	// The view's bearing (the camera's yaw has 0 = east; a bearing 0 = north, clockwise), the
	// soldier's facing, and how close the view is to the head's limit, turning by the rules.
	const int viewBearing = ((int)std::lround(_view->getYawDegrees() + 90.0) % 360 + 360) % 360;
	// While a step is walked the game points the soldier along the step. Shown instead: the
	// facing it keeps (a sidestep, turning by the rules), or with free turning the one it takes
	// again at the end, the compass point the view looks closest to. So the facing on the rim
	// never flicks sideways for a step to the side.
	int facing = unit->getDirection();
	if (_stepPending)
	{
		facing = _sidestepFacing >= 0 ? _sidestepFacing
			: !Options::fpsTurnCost ? (((int)std::lround((_view->getYawDegrees() + 90.0) / 45.0)) % 8 + 8) % 8 : facing;
	}
	int warning = 0;
	if (Options::fpsTurnCost)
	{
		const double offFacing = std::abs(std::remainder(viewBearing - facing * 45.0, 360.0));
		if (offFacing > HeadTurn - CompassWarnDegrees)
		{
			warning = (doomModeOn() || unit->getTimeUnits() >= unit->getTurnCost()) ? 1 : 2;
		}
	}
	// The shot's turn: its cost and the way the soldier will face (none while a step is walked).
	int shotFacing = -1, shotTurn = 0, shotWarning = 0;
	bool shotAffordable = true, meleeTurn = false;
	if (playerTurn && canAct() && !_stepPending)
	{
		shotTurn = shotTurnCost(unit, shotFacing, shotAffordable, &meleeTurn);
		shotWarning = shotTurn ? (shotAffordable ? 1 : 2) : 0;
		if (!shotTurn) shotFacing = -1;
	}
	if (viewBearing != _compassDrawn[0] || facing != _compassDrawn[1] || warning != _compassDrawn[2]
		|| shotFacing != _compassDrawn[3] || (shotTurn * 4 + shotWarning) * 2 + meleeTurn != _compassDrawn[4])
	{
		_compassDrawn[0] = viewBearing;
		_compassDrawn[1] = facing;
		_compassDrawn[2] = warning;
		_compassDrawn[3] = shotFacing;
		_compassDrawn[4] = (shotTurn * 4 + shotWarning) * 2 + meleeTurn;
		drawCompass(viewBearing, facing, warning, shotFacing);

		// The letters, inside the rim, at their bearings measured from the view's.
		const double radius = CompassSize / 2 - 9.0;
		for (int letter = 0; letter < 4; ++letter)
		{
			const double angle = (letter * 90 - viewBearing) * Pi / 180.0;
			const double centreX = _compass->getX() + CompassSize / 2.0 + radius * std::sin(angle);
			const double centreY = _compass->getY() + CompassArrowH + CompassSize / 2.0 - radius * std::cos(angle);
			_compassLetters[letter]->setX((int)std::lround(centreX - 3.5));
			_compassLetters[letter]->setY((int)std::lround(centreY - 3.5));
		}

		// The line under it: the shot's turn first (it's what the next click costs), else the
		// head's limit.
		_txtCompassTurn->setVisible(warning != 0 || shotWarning != 0);
		if (shotWarning)
		{
			_txtCompassTurn->setColor(textColor(_state->getPalette(), shotWarning == 1 ? 250 : 230, shotWarning == 1 ? 160 : 60, shotWarning == 1 ? 60 : 50));
			_txtCompassTurn->setText(_state->tr(meleeTurn ? "STR_FPS_COMPASS_HIT_TURN" : "STR_FPS_COMPASS_SHOT_TURN").arg(shotTurn));
		}
		else if (warning)
		{
			_txtCompassTurn->setColor(textColor(_state->getPalette(), warning == 1 ? 240 : 230, warning == 1 ? 210 : 60, warning == 1 ? 60 : 50));
			_txtCompassTurn->setText(warning == 1 ? std::string(_state->tr("STR_FPS_COMPASS_TURN").arg(unit->getTurnCost())) : std::string(_state->tr("STR_FPS_COMPASS_NO_TU")));
		}
	}
}

/**
 * The lines for the ways of walking switched on with their keys (see toggleSprint and
 * toggleEnPassant), at the top right: under the compass and its line when the compass shows,
 * else where the compass would be. "SPRINT" is green while the soldier runs, and grey while they
 * can't and walk (an armor that doesn't run, or no breath left for a run). Only in 3D, with the
 * HUD shown.
 * @param unit The soldier the view looks through, or null to hide the lines.
 */
void FirstPersonMode::updateWalkModeLines(BattleUnit *unit)
{
	const bool show = isActive() && !_hudHidden && unit;
	_txtSprint->setVisible(show && _sprintOn);
	_txtEnPassant->setVisible(show && _enPassantOn);
	if (!show)
	{
		return;
	}
	// From under the compass's line (shown or not, so the lines don't jump), or from where the
	// compass would be; SPRINT first, then EN PASSANT.
	int lineY = _compass->getVisible() ? _txtCompassTurn->getY() + 10 : _compass->getY();
	if (_sprintOn)
	{
		_txtSprint->setY(lineY);
		const bool runs = _sprintStepRan && unit->getArmor()->allowsRunning(unit->isSmallUnit());
		const Uint8 color = runs ? textColor(_state->getPalette(), 120, 230, 110) : textColor(_state->getPalette(), 150, 150, 160);
		if (_txtSprint->getColor() != color) _txtSprint->setColor(color); // (setting it redraws the text)
		lineY += 9;
	}
	_txtEnPassant->setY(lineY);
}

/**
 * The climb arrows beside the compass (a player: "Compass should have an indicator by it to show
 * if a unit is looking up enough to climb"): where W and S may climb (a grav lift, a ladder, or
 * flying with flight on: see climbWays), an arrow for each way it goes, up above and down below,
 * dim grey; the one W would take now (looking up or down more than ClimbPitch degrees, see
 * updateClimb) lit yellow. None while the soldier can't climb from there, or can't act. Drawn
 * again only when what it shows changes.
 * @param unit The soldier the view looks through, or null to hide them.
 */
void FirstPersonMode::updateClimbArrows(BattleUnit *unit)
{
	bool up = false, down = false;
	if (unit && canAct() && unit->getStatus() == STATUS_STANDING && !_stepPending)
	{
		climbWays(unit, up, down);
	}
	const double pitch = _view->getPitchDegrees();
	const int lit = pitch > ClimbPitch && up ? 1 : pitch < -ClimbPitch && down ? 2 : 0;
	const int shown = (up ? 1 : 0) | (down ? 2 : 0) | (lit << 2);
	_climbArrows->setVisible(shown != 0);
	if (shown == _climbArrowsShown)
	{
		return;
	}
	_climbArrowsShown = shown;
	const SDL_Color *palette = _state->getPalette();
	const Uint8 dim = nearestColor(palette, 110, 115, 130), bright = nearestColor(palette, 250, 220, 70), outline = nearestColor(palette, 8, 8, 8);
	_climbArrows->clear();
	_climbArrows->lock();
	// Each arrow a triangle ClimbArrowW wide, pointing up at the top or down at the bottom, with a
	// dark outline.
	for (int way = 1; way <= 2; ++way)
	{
		if (!((way == 1 && up) || (way == 2 && down))) continue;
		const Uint8 fill = lit == way ? bright : dim;
		for (int row = 0; row < ClimbArrowH; ++row)
		{
			const int halfWidth = row * (ClimbArrowW / 2) / (ClimbArrowH - 1);
			const int y = way == 1 ? row : _climbArrows->getHeight() - 1 - row;
			for (int x = ClimbArrowW / 2 - halfWidth; x <= ClimbArrowW / 2 + halfWidth; ++x)
			{
				const bool edge = row == ClimbArrowH - 1 || x == ClimbArrowW / 2 - halfWidth || x == ClimbArrowW / 2 + halfWidth;
				_climbArrows->setPixel(x, y, edge ? outline : fill);
			}
		}
	}
	_climbArrows->unlock();
}

/**
 * Draws the compass: a dark disc with a grey rim, ticks at the eight directions a soldier can
 * face, the head's free turn as an arc on the rim (turning by the rules), the soldier's facing as
 * a yellow triangle on the rim, the way a shot will turn the soldier as a smaller orange one,
 * and on top of the disc the heading arrow, hugging the rim and pointing in. Angles on the disc
 * go clockwise from its top, which is the view's bearing.
 * @param viewBearing The view's bearing, degrees (0 north, clockwise).
 * @param facing The soldier's facing, 0 (north) to 7, clockwise.
 * @param warning 0, or 1 when the view is near the head's limit, 2 when the soldier couldn't pay for the turn.
 * @param shotFacing The facing a shot at the crosshair will turn the soldier to, 0 to 7; -1 for none.
 */
void FirstPersonMode::drawCompass(int viewBearing, int facing, int warning, int shotFacing)
{
	const SDL_Color *palette = _state->getPalette();
	const Uint8 dark = nearestColor(palette, 20, 22, 30), rim = nearestColor(palette, 150, 155, 175), tick = nearestColor(palette, 190, 195, 210);
	const Uint8 facingColor = nearestColor(palette, 250, 220, 70);
	const Uint8 arcColor = warning == 2 ? nearestColor(palette, 230, 60, 50) : warning == 1 ? nearestColor(palette, 240, 210, 60) : nearestColor(palette, 90, 190, 90);
	const double centre = (CompassSize - 1) / 2.0, centreY = CompassArrowH + centre, radius = CompassSize / 2.0 - 1.0;
	// A point at a bearing (degrees from the view) and a distance from the middle, on the surface.
	auto at = [&](double degreesFromView, double distance, int &x, int &y) {
		const double angle = degreesFromView * Pi / 180.0;
		x = (int)std::lround(centre + distance * std::sin(angle));
		y = (int)std::lround(centreY - distance * std::cos(angle));
	};
	_compass->clear();
	_compass->lock();

	// The disc and its rim.
	for (int y = CompassArrowH; y < CompassArrowH + CompassSize; ++y)
	{
		for (int x = 0; x < CompassSize; ++x)
		{
			const double distance = std::sqrt((x - centre) * (x - centre) + (y - centreY) * (y - centreY));
			if (distance > radius + 0.5) continue;
			_compass->setPixel(x, y, distance > radius - 1.0 ? rim : dark);
		}
	}
	// Turning by the rules: the head's free turn, an arc along the rim either side of the facing.
	if (Options::fpsTurnCost)
	{
		for (double degrees = -HeadTurn; degrees <= HeadTurn; degrees += 2.0)
		{
			for (double distance = radius - 2.0; distance <= radius; distance += 1.0)
			{
				int x, y;
				at(facing * 45.0 - viewBearing + degrees, distance, x, y);
				_compass->setPixel(x, y, arcColor);
			}
		}
	}
	// The eight directions a soldier faces: short ticks in from the rim.
	for (int direction = 0; direction < 8; ++direction)
	{
		for (double distance = radius - (direction % 2 ? 3.0 : 4.0); distance <= radius - 1.0; distance += 1.0)
		{
			int x, y;
			at(direction * 45.0 - viewBearing, distance, x, y);
			_compass->setPixel(x, y, tick);
		}
	}
	// The soldier's facing: a yellow triangle on the rim pointing in, outside the letters; the way
	// a shot will turn the soldier, a smaller orange one.
	auto rimTriangle = [&](int direction, double size, Uint8 color) {
		const double angle = (direction * 45.0 - viewBearing) * Pi / 180.0;
		for (double distance = radius - size; distance <= radius; distance += 0.5)
		{
			const double halfWidth = (distance - (radius - size)) * 0.45;
			for (double side = -halfWidth; side <= halfWidth; side += 0.5)
			{
				const int x = (int)std::lround(centre + distance * std::sin(angle) + side * std::cos(angle));
				const int y = (int)std::lround(centreY - distance * std::cos(angle) + side * std::sin(angle));
				_compass->setPixel(x, y, color);
			}
		}
	};
	rimTriangle(facing, 6.0, facingColor);
	if (shotFacing >= 0)
	{
		rimTriangle(shotFacing, 5.0, nearestColor(palette, 250, 140, 40));
	}
	// The heading: a fixed arrow on top of the disc, pointing in, its tip on the rim and its sides
	// stopping at the rim, so it hugs the disc; bright, outlined dark so it stands out on the sky.
	const Uint8 arrow = nearestColor(palette, 245, 245, 250), outline = nearestColor(palette, 10, 10, 14);
	const int middle = (int)std::lround(centre);
	for (int row = 0; row <= CompassArrowH + 1; ++row)
	{
		const int halfWidth = (CompassArrowH + 1 - row) * 6 / (CompassArrowH + 1);
		for (int column = -halfWidth - 1; column <= halfWidth + 1; ++column)
		{
			const int x = middle + column, y = row;
			const double toCentre = std::sqrt((x - centre) * (x - centre) + (y - centreY) * (y - centreY));
			if (toCentre < radius - 2.0) continue;
			const bool edge = std::abs(column) > halfWidth || row == 0;
			_compass->setPixel(x, y, edge ? outline : arrow);
		}
	}
	_compass->unlock();
}

/**
 * Makes the surface the scope cross is drawn on (shown while zoomed in, see updateScope): as big
 * as the canvas, just above the weapon in hand and behind the rest of the screen (made again
 * when the canvas changes size). The cross is drawn once: a thin blue line across the whole
 * view each way through the reticle, with a gap round the reticle itself (ScopeGap) and short
 * ticks every ScopeTickSpacing pixels, like a rifle scope's, unobtrusive.
 */
void FirstPersonMode::makeScopeCross()
{
	const int width = canvasWidth(), height = canvasHeight();
	Surface *surface = new Surface(width, height, 0, 0);
	surface->setPalette(_state->getPalette());
	surface->setVisible(false);
	auto &surfaces = _state->_surfaces;
	if (_scopeCross)
	{
		// Made again (a resize): the new surface takes the old one's place in the screen's lists.
		replaceStateSurface(_scopeCross, surface);
		_ownSurfaces.erase(_scopeCross);
		delete _scopeCross;
	}
	else
	{
		// The first time: straight after the weapon in hand in the screen's drawing order, and
		// the screen's to delete at its end.
		surfaces.insert(std::find(surfaces.begin(), surfaces.end(), _weaponInHand) + 1, surface);
		_state->_surfacesOwned.push_back(surface);
	}
	_ownSurfaces.insert(surface);
	_scopeCross = surface;

	// The cross, from the middle of the canvas (the reticle's middle) out to the edges, in a
	// soft blue; the ticks a step brighter.
	const SDL_Color *palette = _state->getPalette();
	const Uint8 lineColor = nearestColor(palette, 70, 120, 210), tickColor = nearestColor(palette, 110, 160, 240);
	const int middleX = width / 2, middleY = height / 2;
	surface->lock();
	for (int x = 0; x < width; ++x)
	{
		const int fromMiddle = std::abs(x - middleX);
		if (fromMiddle < ScopeGap) continue;
		surface->setPixel(x, middleY, lineColor);
		if (fromMiddle % ScopeTickSpacing == 0)
		{
			surface->setPixel(x, middleY - 1, tickColor);
			surface->setPixel(x, middleY + 1, tickColor);
		}
	}
	for (int y = 0; y < height; ++y)
	{
		const int fromMiddle = std::abs(y - middleY);
		if (fromMiddle < ScopeGap) continue;
		surface->setPixel(middleX, y, lineColor);
		if (fromMiddle % ScopeTickSpacing == 0)
		{
			surface->setPixel(middleX - 1, y, tickColor);
			surface->setPixel(middleX + 1, y, tickColor);
		}
	}
	surface->unlock();
}

/**
 * Zoomed in (setting "Zoom"): shows the scope cross through the reticle, and at the right, in
 * blue, SCOPING with how much closer the view looks right now ("SCOPING 2.6x"), the number
 * climbing as the zoom glides in and falling as it glides out (see updateZoom). Both show from
 * a little past 1x, and not with the HUD hidden.
 */
void FirstPersonMode::updateScope()
{
	const bool scoping = isActive() && !_hudHidden && _zoom > 1.05;
	_scopeCross->setVisible(scoping);
	_txtScoping->setVisible(scoping);
	if (!scoping)
	{
		return;
	}

	// The zoom with one decimal, or a whole number when it is one ("4x" once all the way in).
	const double shownZoom = std::round(_zoom * 10.0) / 10.0;
	std::ostringstream zoomText;
	if (shownZoom == std::round(shownZoom)) zoomText << (int)shownZoom;
	else zoomText << std::fixed << std::setprecision(1) << shownZoom;
	_txtScoping->setColor(textColor(_state->getPalette(), 110, 170, 255));
	_txtScoping->setText(_state->tr("STR_FPS_SCOPING").arg(zoomText.str()));
}

/**
 * The classic HUD in 3D (shown with H): its buttons that act on the isometric map, and their
 * keys, translated to the 3D view. The rest of the HUD does the same in both views and is left
 * to the game: kneel, inventory, next soldier (and next with "done"), options, end turn, abort,
 * the soldier's stats, the hands and their action menus (the action picked is then aimed in 3D,
 * see fire), the time unit reserves and the special and skill buttons.
 * - Soldier up / down: as the up and down keys in 3D (moveUpDown: a grav lift, or flight, which
 *   must be switched on).
 * - Map level up / down: the map would show the level above or below, which the 3D view shows
 *   anyway; the view looks towards it instead (lookLevelUpDown).
 * - Minimap: opens on the soldier (the map's camera, which it shows, goes there first).
 * - Centre on the soldier: the view turns back to where the soldier faces, level
 *   (lookStraightAhead).
 * (The visible enemy buttons are turned into looks at the enemy by handleFreeCursor, and the
 * map's multi-level view is explained by watchMapLayers.)
 * @param button The classic HUD's button.
 * @param action The input event, passed on to the game's handler where one is used.
 * @return True if the button is one of those (and was handled).
 */
bool FirstPersonMode::classicHudButton(const InteractiveSurface *button, Action *action)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (button == _state->_btnUnitUp || button == _state->_btnUnitDown)
	{
		moveUpDown(unit, button == _state->_btnUnitUp ? Pathfinding::DIR_UP : Pathfinding::DIR_DOWN, action);
		return true;
	}
	if (button == _state->_btnMapUp || button == _state->_btnMapDown)
	{
		lookLevelUpDown(button == _state->_btnMapUp ? 1 : -1);
		return true;
	}
	if (button == _state->_btnShowMap)
	{
		if (unit)
		{
			_state->_map->getCamera()->centerOnPosition(unit->getPosition());
		}
		_state->btnShowMapClick(action);
		return true;
	}
	if (button == _state->_btnCenter)
	{
		// The soldier the view looks through (on the aliens' turn, the last of ours).
		if (_trackedUnit && !_trackedUnit->isOut())
		{
			lookStraightAhead(_trackedUnit);
		}
		return true;
	}
	return false;
}

/**
 * The map's multi-level view (the classic HUD's layers button with OXCE's links menu switched
 * off, its key, or that menu's "Multi-level view") only changes what the isometric map draws:
 * the 3D view always shows every level. Switched while the 3D view shows, a warning line says
 * so. Called every frame, in both views.
 */
void FirstPersonMode::watchMapLayers()
{
	const bool showsAll = _state->_map->getCamera()->getShowAllLayers();
	if (showsAll != _mapShowsAllLayers && isActive())
	{
		_state->warning(showsAll ? "STR_FPS_MAP_LAYERS_ALL" : "STR_FPS_MAP_LAYERS_ONE");
	}
	_mapShowsAllLayers = showsAll;
}

/**
 * Fills the info panel at the bottom left of the 3D view: the soldier's name and status
 * (kneeling, wounds, burning...), a line for the weapon in hand and one for the other hand's
 * item (each gun with the rounds loaded and the spare clips carried for it, see ammoLabel), and
 * the four stats with their bars (time units, energy, health with the stun level over it,
 * morale). It is filled anew every frame, so the counts follow every shot, reload, pick-up and
 * drop.
 * The panel only shows in the 3D view without the classic HUD (which has all of this itself).
 * @param unit The selected soldier, or null for none (the panel hides).
 */
void FirstPersonMode::updateInfoPanel(BattleUnit *unit)
{
	// Show or hide every piece of the panel together. The cheat tag shows only while a cheat is
	// on: DOOM MODE, or the extras that are on (IDDQD, IDKFA).
	const bool show = isActive() && !_classicHud && !_hudHidden && unit;
	for (auto *part : _panelParts)
	{
		part->setVisible(show);
	}
	_txtDoom->setVisible(show && doomModeOn());
	if (!show)
	{
		return;
	}
	const std::string cheats = cheatTag();
	if (_txtDoom->getText() != cheats)
	{
		_txtDoom->setText(cheats);
	}
	// The rank (a soldier's) and the name after it (the owner: before the name, less in the way),
	// and the status.
	const bool panelRank = drawRankIcon(_panelRank, unit);
	_panelRank->setVisible(panelRank);
	_panelRank->setX(_panelNameX);
	_panelRank->setY(_txtName->getY() - RankIconRise);
	_txtName->setX(_panelNameX + (panelRank ? RankIconW + RankIconGap : 0));
	_txtName->setText(unit->getName(_game->getLanguage(), false));
	_txtStatus->setText(statusLabel(unit));

	// The hands: the weapon that fires (in the fire mode picked for it, when that's the weapon
	// the mode was picked for), then whatever is in the other hand (with the built-in weapon in
	// use, the right hand's item, or else the left's). The other hand's line is shown as active
	// too when its gun fires along (dual wield).
	BattleItem *weapon = activeWeapon(unit);
	const int hand = activeHand(unit);
	BattleItem *otherItem = hand == 1 ? unit->getRightHandWeapon() : hand == 0 ? unit->getLeftHandWeapon()
		: (unit->getRightHandWeapon() ? unit->getRightHandWeapon() : unit->getLeftHandWeapon());
	if (otherItem == weapon)
	{
		otherItem = nullptr; // (an item held in both hands)
	}
	fillHandLine(_txtWeapon, _txtWeaponAmmo, unit, weapon, weapon && weapon == _fireModeWeapon ? _fireMode : BA_NONE, true);
	fillHandLine(_txtOtherWeapon, _txtOtherAmmo, unit, otherItem, BA_NONE, otherItem && otherItem == offHandWeapon(unit, weapon));
	// The four stats, each as a number and as a bar measured against the soldier's maximum (its
	// scale by the setting "Stat bars", see statBarScale).
	_numTimeUnits->setValue(unit->getTimeUnits());
	_barTimeUnits->setMax(unit->getBaseStats()->tu);
	_barTimeUnits->setValue(unit->getTimeUnits());
	_numEnergy->setValue(unit->getEnergy());
	_barEnergy->setMax(unit->getBaseStats()->stamina);
	_barEnergy->setValue(unit->getEnergy());
	_numHealth->setValue(std::max(0, unit->getHealth())); // (an overkill leaves it below zero)
	_barHealth->setMax(unit->getBaseStats()->health);
	_barHealth->setValue(std::max(0, unit->getHealth()));
	_barHealth->setValue2(unit->getStunlevel()); // stun damage, drawn over the health bar as in the game
	_numMorale->setValue(unit->getMorale());
	_barMorale->setMax(100);
	_barMorale->setValue(unit->getMorale());
	const int panelMaxima[4] = { unit->getBaseStats()->tu, unit->getBaseStats()->stamina, unit->getBaseStats()->health, 100 };
	Bar *panelBars[4] = { _barTimeUnits, _barEnergy, _barHealth, _barMorale };
	for (int stat = 0; stat < 4; ++stat)
	{
		panelBars[stat]->setScale(statBarScale(_state, stat, panelMaxima[stat], panelBars[stat]->getWidth()));
	}

	// Doom IDDQD: an invincible soldier's stats are silver; otherwise in their own colours.
	const bool silver = invincible(_state, unit);
	if (silver != _statsSilver)
	{
		_statsSilver = silver;
		const Uint8 silverColor = nearestColor(_state->getPalette(), 215, 215, 225);
		NumberText *numbers[4] = { _numTimeUnits, _numEnergy, _numHealth, _numMorale };
		Bar *bars[4] = { _barTimeUnits, _barEnergy, _barHealth, _barMorale };
		for (int stat = 0; stat < 4; ++stat)
		{
			numbers[stat]->setColor(silver ? silverColor : _statColors[stat]);
			bars[stat]->setColor(silver ? silverColor : _statColors[4 + stat]);
		}
	}
}

/**
 * Fills one hand's line of the info panel: which hand ("R:" or "L:") and the item's name, and
 * for a gun its ammo right after it (see ammoLabel), such as "R: Rifle 17 | 2 clips (40)". A
 * name too long for the room the ammo leaves on the line is shortened. The line of the weapon
 * that fires is in the weapons' colour; the other hand's line is grey unless it fires along
 * (dual wield). The ammo is red while the gun has no rounds loaded for the mode. The colours
 * are group starts (textColor), as the glyph shades need.
 * @param nameText The line's first text (the hand and the name), at the line's left end.
 * @param ammoText The line's second text (the ammo), placed right after the name.
 * @param unit The soldier.
 * @param item The item in that hand, or null for an empty hand (the line stays empty).
 * @param mode The fire mode whose ammo to show (BA_NONE: the gun's first shot's).
 * @param active True if this hand's weapon fires: the line in the weapons' colour.
 */
void FirstPersonMode::fillHandLine(Text *nameText, Text *ammoText, BattleUnit *unit, BattleItem *item, BattleActionType mode, bool active)
{
	// An empty hand: an empty line.
	if (!item)
	{
		nameText->setText("");
		ammoText->setText("");
		return;
	}

	// The ammo first: the name gets the room it leaves on the line (with a gap of 5 pixels
	// between them), and the ammo goes right after the name.
	const int gap = 5;
	bool loaded = true;
	int tagColor = -1;
	const std::string ammo = ammoLabel(unit, item, mode, loaded, &tagColor);
	const int nameRoom = PanelLineW - (ammo.empty() ? 0 : smallTextWidth(ammo) + gap);
	const std::string name = fitText(handLetter(unit, item) + std::string(_state->tr(item->getRules()->getName())), nameRoom);
	if (nameText->getText() != name) nameText->setText(name);
	if (ammoText->getText() != ammo) ammoText->setText(ammo);
	ammoText->setX(nameText->getX() + smallTextWidth(name) + gap);

	// The colours (setting one redraws the text, so only on a change).
	const SDL_Color *palette = _state->getPalette();
	const Uint8 lineColor = active ? _weaponTextColor : textColor(palette, 160, 160, 160);
	const Uint8 ammoColor = !loaded ? textColor(palette, 240, 70, 50) : lineColor;
	if (nameText->getColor() != lineColor) nameText->setColor(lineColor);
	if (ammoText->getColor() != ammoColor) ammoText->setColor(ammoColor);
	if (tagColor >= 0 && ammoText->getSecondaryColor() != tagColor) ammoText->setSecondaryColor((Uint8)tagColor); // (the ammo tag, see ammoTag)
}

/**
 * Works out a gun's ammo as the info panel shows it: the rounds loaded for a fire mode, then
 * the spare clips the soldier carries for it and their rounds together, such as
 * "17 | 2 clips (40)". By the game's own rules:
 * - the rounds are those in the clip loaded in the ammo slot the fire mode uses (a gun can have
 *   several slots); "EMPTY" when that slot has no clip (the game drops a clip once it's spent);
 * - a gun with its rounds built in (no clips) shows its own count, or "∞" when it never runs
 *   out (the game's clip size -1, like the laser weapons), and nothing about clips;
 * - the spare clips are the items in the soldier's inventory (belt, pockets, backpack, the
 *   other hand...) that the gun takes in that slot, by the game's list of compatible ammo (as
 *   reloading and the inventory use it), of whatever kind (armor piercing, incendiary...);
 * - a gun that takes clips of several damage types in that slot says the loaded one's after
 *   its rounds, in two letters in its colour (see ammoTag): "6 [HE] | 2 clips (12)".
 * @param unit The soldier carrying the gun.
 * @param weapon The item.
 * @param mode The fire mode whose ammo slot to show (BA_NONE: the gun's first shot's).
 * @param loaded Set to false when the gun has no rounds loaded for that mode.
 * @param tagColor If not null, set to the ammo tag's colour (the text's secondary colour: the tag
 *   sits between two Unicode::TOK_COLOR_FLIP), a palette index; left alone without a tag.
 * @return The ammo text; empty for an item that isn't a gun.
 */
std::string FirstPersonMode::ammoLabel(const BattleUnit *unit, const BattleItem *weapon, BattleActionType mode, bool &loaded, int *tagColor) const
{
	loaded = true;
	const RuleItem *rules = weapon->getRules();
	if (rules->getBattleType() != BT_FIREARM)
	{
		// A primed grenade (or anything with a fuse): PRIMED and the turns its fuse has left, said
		// so ("PRIMED 3 turns"; 0: "PRIMED this turn", it goes off at the end of this turn, once it
		// lies on the ground; the owner, after primed 3 was taken for seconds); one that goes off
		// on landing just PRIMED.
		if (rules->getFuseTimerType() != BFT_NONE && weapon->getFuseTimer() >= 0)
		{
			// Doom mode's real-time fuse: armed and still in hand, its fuse ("FUSE 3s"); burning,
			// the seconds it has left, to a tenth.
			auto doomFuse = _doomFuses.find(weapon->getId());
			if (doomFuse != _doomFuses.end())
			{
				if (!doomFuse->second.lit) return _state->tr("STR_FPS_DOOM_FUSE").arg((int)(doomFuse->second.length / 1000));
				const int tenthsLeft = std::max(0, ((int)doomFuse->second.length - (int)(SDL_GetTicks() - doomFuse->second.lit)) / 100);
				return _state->tr("STR_FPS_PRIMED_SECONDS").arg(std::to_string(tenthsLeft / 10) + "." + std::to_string(tenthsLeft % 10));
			}
			if (rules->getFuseTimerType() == BFT_INSTANT)
			{
				return _state->tr("STR_FPS_PRIMED_INSTANT");
			}
			const int turnsLeft = weapon->getFuseTimer();
			return turnsLeft == 0 ? _state->tr("STR_FPS_PRIMED_THIS_TURN") : turnsLeft == 1 ? _state->tr("STR_FPS_PRIMED_ONE_TURN") : _state->tr("STR_FPS_PRIMED").arg(turnsLeft);
		}
		// Doom mode's real-time grenades primed on throw: one on a timer, not armed yet, burns for
		// the fuse once thrown.
		if (doomModeOn() && Options::fpsDoomPrimeOnThrow && timedGrenade(weapon))
		{
			return _state->tr("STR_FPS_DOOM_FUSE").arg(std::clamp(Options::fpsDoomGrenadeFuse, 1, FirstPerson::DoomFuseMaxSeconds));
		}
		return "";
	}

	// The ammo slot: the fire mode's, or for no shot mode the first shot the gun has (snap,
	// aimed, auto, as the action menu lists them; a guided launcher's is its launch).
	BattleActionType slotMode = mode;
	if (!isShot(slotMode) && slotMode != BA_LAUNCH)
	{
		slotMode = rules->getCostSnap().Time > 0 ? BA_SNAPSHOT : rules->getCostAimed().Time > 0 ? BA_AIMEDSHOT : BA_AUTOSHOT;
	}
	const int slot = weapon->getActionConf(slotMode)->ammoSlot;

	// A shot that uses no ammo at all never runs out.
	if (slot == RuleItem::AmmoSlotSelfUse)
	{
		return _state->tr("STR_FPS_AMMO_UNLIMITED");
	}

	// The rounds loaded: the clip's (or the gun's own), "∞" for a clip that never runs out.
	const BattleItem *clip = weapon->getAmmoForSlot(slot);
	std::string text;
	if (!clip)
	{
		text = _state->tr("STR_FPS_AMMO_EMPTY");
		loaded = false;
	}
	else if (clip->getRules()->getClipSize() < 0)
	{
		text = _state->tr("STR_FPS_AMMO_UNLIMITED");
	}
	else
	{
		text = std::to_string(clip->getAmmoQuantity());
		loaded = clip->getAmmoQuantity() > 0;
	}

	// The loaded clip's damage type, for a gun that takes several (GitHub issue #51).
	Uint8 color = 0;
	const std::string tag = ammoTag(weapon, slot, clip, color);
	if (!tag.empty())
	{
		text += std::string(" ") + Unicode::TOK_COLOR_FLIP + "[" + tag + "]" + Unicode::TOK_COLOR_FLIP;
		if (tagColor) *tagColor = color;
	}

	// Rounds built into the gun: no clips to count.
	if (rules->getCompatibleAmmoForSlot(slot)->empty())
	{
		return text;
	}

	// The spare clips carried for that slot, and their rounds.
	int spareClips = 0, spareRounds = 0;
	for (const BattleItem *item : *unit->getInventory())
	{
		if (item != weapon && rules->getSlotForAmmo(item->getRules()) == slot)
		{
			++spareClips;
			spareRounds += std::max(0, item->getAmmoQuantity());
		}
	}
	return text + " | " + std::string(_state->tr("STR_FPS_SPARE_CLIPS", spareClips).arg(spareRounds));
}

/**
 * Names the damage type of the clip loaded in a gun's ammo slot in two letters, for a gun that
 * takes clips of more than one damage type there (a player, GitHub issue #51: whether a cannon has
 * normal, explosive or incendiary loaded, at a glance; the owner: two letters in brackets, only for
 * such guns, coloured): the heavy cannon and auto-cannon (AP, HE, IN), the rocket launcher (small
 * and large rockets HE, incendiary IN), TFTD's gas cannon, hydro-jet and torpedo launcher; a rifle
 * or pistol (one type) gets none. The letters come from the type's name as the Ufopaedia gives it
 * in the game's language (a mod's own names included): the first letters of its first two words,
 * or a one-word name's first two letters ("ARMOR PIERCING" AP, "HIGH EXPLOSIVE" HE, "INCENDIARY"
 * IN, "LASER BEAM" LB, X-Com Files' "ELECTRIC" EL); a mod's own type it doesn't name, its number
 * (10 to 19). The colour is the type's (see damageTypeColor).
 * @param weapon The gun.
 * @param slot Its ammo slot.
 * @param clip The clip loaded there, or null for none (no tag).
 * @param color Set to the tag's colour (a group start, see textColor).
 * @return The two letters; empty for no tag.
 */
std::string FirstPersonMode::ammoTag(const BattleItem *weapon, int slot, const BattleItem *clip, Uint8 &color) const
{
	if (!clip || !clip->getRules()->getDamageType())
	{
		return "";
	}

	// Only for a gun whose clips for that slot do more than one type of damage.
	const ItemDamageType type = clip->getRules()->getDamageType()->ResistType;
	const std::vector<const RuleItem*> *compatible = weapon->getRules()->getCompatibleAmmoForSlot(slot);
	bool severalTypes = false;
	for (const RuleItem *ammo : *compatible)
	{
		severalTypes |= ammo->getDamageType() && ammo->getDamageType()->ResistType != type;
	}
	if (!severalTypes || type <= DT_NONE || type >= DAMAGE_TYPES)
	{
		return "";
	}

	// The letters, from the type's name (the Ufopaedia's strings, as ArticleState::getDamageTypeText).
	static const char *const typeNames[DAMAGE_TYPES] = { "STR_DAMAGE_NONE", "STR_DAMAGE_ARMOR_PIERCING", "STR_DAMAGE_INCENDIARY",
		"STR_DAMAGE_HIGH_EXPLOSIVE", "STR_DAMAGE_LASER_BEAM", "STR_DAMAGE_PLASMA_BEAM", "STR_DAMAGE_STUN", "STR_DAMAGE_MELEE",
		"STR_DAMAGE_ACID", "STR_DAMAGE_SMOKE", "STR_DAMAGE_10", "STR_DAMAGE_11", "STR_DAMAGE_12", "STR_DAMAGE_13", "STR_DAMAGE_14",
		"STR_DAMAGE_15", "STR_DAMAGE_16", "STR_DAMAGE_17", "STR_DAMAGE_18", "STR_DAMAGE_19" };
	const std::string name = _state->tr(typeNames[type]);
	std::string tag;
	if (name == typeNames[type])
	{
		tag = std::to_string((int)type); // (a mod's own type with no name)
	}
	else
	{
		// The words, split at spaces and hyphens.
		std::vector<UString> words(1);
		for (UCode character : Unicode::convUtf8ToUtf32(name))
		{
			if (character == ' ' || character == '-') { if (!words.back().empty()) words.emplace_back(); }
			else words.back() += character;
		}
		if (words.back().empty()) words.pop_back();
		UString letters;
		if (words.size() >= 2) letters = { words[0][0], words[1][0] };
		else if (!words.empty()) letters = words[0].substr(0, 2);
		tag = Unicode::convUtf32ToUtf8(letters);
		Unicode::upperCase(tag);
	}

	color = damageTypeColor(type);
	return tag;
}

/**
 * Makes the weapon line of the soldier card: the weapon in the hand that fires, with its ammo
 * (see ammoLabel), such as "R: Rifle 17 | 2 clips (40)" ("L:" for the left hand), the name
 * shortened if the line would not fit the card.
 * @param unit The soldier whose weapon to describe.
 * @param tagColor If not null, set to the ammo tag's colour, if there is one (see ammoLabel).
 * @return The weapon line, empty if the soldier's hands are empty.
 */
std::string FirstPersonMode::weaponLabel(BattleUnit *unit, int *tagColor) const
{
	BattleItem *weapon = activeWeapon(unit);
	if (!weapon)
	{
		return "";
	}
	// The ammo, and the hand and name in the room it leaves on the card (with a space between).
	bool loaded = true;
	const std::string ammo = ammoLabel(unit, weapon, BA_NONE, loaded, tagColor);
	const int nameRoom = CardW - 8 - (ammo.empty() ? 0 : smallTextWidth(" " + ammo));
	const std::string name = fitText(handLetter(unit, weapon) + std::string(_state->tr(weapon->getRules()->getName())), nameRoom);
	return ammo.empty() ? name : name + " " + ammo;
}

/**
 * Shortens a text until it fits a width in the small font: it loses letters from its end and
 * ends in "." instead (a text that fits is left as it is).
 * @param text The text (UTF-8).
 * @param maxWidth The room, in pixels.
 * @return The text, shortened if needed.
 */
std::string FirstPersonMode::fitText(const std::string &text, int maxWidth) const
{
	if (smallTextWidth(text) <= maxWidth)
	{
		return text;
	}
	// Letter by letter (whole characters, not bytes) until it fits with the "." added; spaces
	// left at the end go too.
	UString letters = Unicode::convUtf8ToUtf32(text);
	while (!letters.empty())
	{
		letters.pop_back();
		while (!letters.empty() && letters.back() == ' ') letters.pop_back();
		const std::string shortened = Unicode::convUtf32ToUtf8(letters) + ".";
		if (smallTextWidth(shortened) <= maxWidth)
		{
			return shortened;
		}
	}
	return "";
}

/**
 * Measures a text as the game's Text draws it in the small font: the width of each character
 * (as the font measured its glyph) added up.
 * @param text The text (UTF-8).
 * @return Its width, in pixels.
 */
int FirstPersonMode::smallTextWidth(const std::string &text) const
{
	const Font *font = _game->getMod()->getFont("FONT_SMALL");
	int width = 0;
	for (UCode character : Unicode::convUtf8ToUtf32(text))
	{
		if (character == (UCode)Unicode::TOK_COLOR_FLIP) continue; // (a colour change, see ammoTag)
		width += font->getCharSize(character).w;
	}
	return width;
}

/**
 * Makes the status line of the info panel and the soldier card: the one thing most worth
 * knowing about the soldier right now. In order: a breakdown this turn (panic or berserk: the
 * soldier is out of the player's hands for the turn), then being on fire, then fatal wounds
 * (they bleed every turn and need a medikit), else kneeling.
 * @param unit The soldier to describe.
 * @return The translated status, or an empty string if there's nothing to say.
 */
std::string FirstPersonMode::statusLabel(BattleUnit *unit) const
{
	// A panic or berserk breakdown in this very turn (remembered per soldier by breakdownCam).
	const auto breakdown = _breakdownTurns.find(unit);
	if (breakdown != _breakdownTurns.end() && breakdown->second.first == _save->getTurn())
	{
		return _state->tr(breakdown->second.second ? "STR_FPS_BERSERK" : "STR_FPS_PANIC");
	}
	// On fire.
	if (unit->getFire() > 0)
	{
		return _state->tr("STR_FPS_BURNING");
	}
	// Bleeding: how many fatal wounds.
	if (unit->getFatalWounds() > 0)
	{
		return _state->tr("STR_FPS_WOUNDS").arg(unit->getFatalWounds());
	}
	// Otherwise only kneeling is worth a word.
	return unit->isKneeled() ? std::string(_state->tr("STR_FPS_KNEELING")) : std::string();
}

/**
 * Keeps the armor's part of the top bar up to date: the armor's name, and a button for each
 * thing the armor and the soldier's gear can do. FLY (SWIM under water) for armor that flies,
 * LIGHT for the armor's personal light, NIGHT VISION for a soldier who has it (see
 * hasNightVision), SPECIAL for a weapon built into the armor. A button shows pressed while its
 * feature is switched on. When a button appears or disappears, the squad buttons are laid out
 * again to make room.
 * @param unit The selected soldier, or null for none (all the buttons hide).
 */
void FirstPersonMode::updateArmorButtons(BattleUnit *unit)
{
	// Which buttons this soldier's armor and gear call for (none outside the 3D view).
	const bool show = isActive() && unit;
	BattleType specialType;
	const bool showFly = show && canFly(unit), showLight = show && unit->getArmor()->getPersonalLightFriend() > 0;
	const bool showSpecial = show && unit->getSpecialIconWeapon(specialType) != nullptr;
	const bool showNight = show && hasNightVision(unit);
	// Show or hide them; any change moves the other buttons on the bar.
	if (showFly != _btnFly->getVisible() || showLight != _btnLight->getVisible()
		|| showNight != _btnNight->getVisible() || showSpecial != _btnSpecial->getVisible())
	{
		_btnFly->setVisible(showFly);
		_btnLight->setVisible(showLight);
		_btnNight->setVisible(showNight);
		_btnSpecial->setVisible(showSpecial);
		layoutTopBar(); // the squad buttons make room
	}
	if (!show)
	{
		return;
	}
	// The armor's name ("no armor" for the game's placeholder armor). The armors of tanks and of
	// aliens (one under mind control) have no name in the language files: none is shown then,
	// rather than the rules' id ("TANK_ARMOR").
	const std::string &armorType = unit->getArmor()->getType();
	const std::string armorName = armorType == "STR_NONE_UC" ? std::string(_state->tr("STR_FPS_NO_ARMOR")) : std::string(_state->tr(armorType));
	_txtArmor->setText(armorName == armorType ? std::string() : armorName);
	// A toggle button shows pressed when its group pointer points at it. Work out which are on,
	// and redraw the buttons only when that changed.
	TextButton *flyGroup = flightOn(unit) ? _btnFly : nullptr;
	TextButton *lightGroup = _save->getTogglePersonalLight() ? _btnLight : nullptr;
	TextButton *nightGroup = _state->_map->isNightVisionOn() ? _btnNight : nullptr;
	if (flyGroup != _flyButtonGroup || lightGroup != _lightButtonGroup || nightGroup != _nightButtonGroup)
	{
		_flyButtonGroup = flyGroup;
		_lightButtonGroup = lightGroup;
		_nightButtonGroup = nightGroup;
		_btnFly->invalidate();
		_btnLight->invalidate();
		_btnNight->invalidate();
	}
}

/**
 * Draws a soldier's rank into an icon beside their name (the owner's ask: "the RANK mini symbol
 * of a unit on the right to the unit name", then before it: "less obtrusive"; the info panel's,
 * the soldier card's, the soldier cam's title): a mod's own tiny rank (TinyRanks, 7x7, as XPiratez has; stock OXCE's set is an
 * empty placeholder), or else the game's own battlescape rank badge averaged down to the icon's
 * size (see smallRankBadge; the owner chose it from previews). Drawn again only when the rank
 * shown changes. Aliens, civilians and the like have no rank to show.
 * @param icon The icon (RankIconW by RankIconH).
 * @param unit The unit whose name it follows, or null.
 * @return True if there is a rank to show.
 */
bool FirstPersonMode::drawRankIcon(Surface *icon, const BattleUnit *unit)
{
	// The picture: a mod's tiny rank if it drew one, else the small badge.
	const Soldier *soldier = unit ? unit->getGeoscapeSoldier() : nullptr;
	Surface *picture = nullptr;
	int key = -1; // which picture, to draw it only when it changes (a tiny rank's sprite, or 1000 and the badge's)
	if (soldier)
	{
		SurfaceSet *tinyRanks = _game->getMod()->getSurfaceSet("TinyRanks", false);
		Surface *tiny = tinyRanks ? tinyRanks->getFrame(soldier->getRankSpriteTiny()) : nullptr;
		bool drawn = false;
		for (int y = 0; tiny && !drawn && y < tiny->getHeight(); ++y)
		{
			for (int x = 0; !drawn && x < tiny->getWidth(); ++x) drawn = tiny->getPixel(x, y) != 0;
		}
		if (drawn)
		{
			picture = tiny;
			key = soldier->getRankSpriteTiny();
		}
		else if ((picture = smallRankBadge(soldier->getRankSpriteBattlescape())))
		{
			key = 1000 + soldier->getRankSpriteBattlescape();
		}
	}
	auto shown = _rankDrawn.emplace(icon, -2).first; // (-2: never drawn)
	if (shown->second != key)
	{
		shown->second = key;
		icon->clear();
		if (picture)
		{
			picture->blitNShade(icon, 0, (RankIconH - std::min(RankIconH, picture->getHeight())) / 2); // (a tiny rank in the middle of the height)
		}
	}
	return picture != nullptr;
}

/**
 * The game's own battlescape rank badge (SMOKE.PCK, 26x23 in the original, as the classic HUD
 * shows it), averaged down to RankIconW by RankIconH: each of its pixels the average colour of the
 * badge's pixels under it, matched to the nearest colour of the palette (a corner mostly
 * transparent stays so). Made once per badge.
 * @param sprite The badge's frame in SMOKE.PCK.
 * @return The small badge, or null if the frame isn't there.
 */
Surface *FirstPersonMode::smallRankBadge(int sprite)
{
	auto made = _smallRankBadges.find(sprite);
	if (made != _smallRankBadges.end())
	{
		return made->second.get();
	}
	SurfaceSet *smoke = _game->getMod()->getSurfaceSet("SMOKE.PCK", false);
	Surface *badge = smoke ? smoke->getFrame(sprite) : nullptr;
	Surface *small = nullptr;
	if (badge)
	{
		// The badge's own box (the frame is bigger, transparent round it).
		int left = badge->getWidth(), top = badge->getHeight(), right = -1, bottom = -1;
		for (int y = 0; y < badge->getHeight(); ++y)
		{
			for (int x = 0; x < badge->getWidth(); ++x)
			{
				if (badge->getPixel(x, y) == 0) continue;
				left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y);
			}
		}
		if (right >= left)
		{
			const SDL_Color *palette = _state->getPalette();
			const int badgeW = right - left + 1, badgeH = bottom - top + 1;
			small = new Surface(RankIconW, RankIconH, 0, 0);
			small->setPalette(_state->getPalette());
			small->lock();
			for (int smallY = 0; smallY < RankIconH; ++smallY)
			{
				for (int smallX = 0; smallX < RankIconW; ++smallX)
				{
					// The badge's pixels under this one, their average colour.
					const int x0 = left + smallX * badgeW / RankIconW, x1 = left + std::max(smallX * badgeW / RankIconW + 1, (smallX + 1) * badgeW / RankIconW);
					const int y0 = top + smallY * badgeH / RankIconH, y1 = top + std::max(smallY * badgeH / RankIconH + 1, (smallY + 1) * badgeH / RankIconH);
					int red = 0, green = 0, blue = 0, count = 0, cells = 0;
					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							++cells;
							const Uint8 index = badge->getPixel(x, y);
							if (index == 0) continue;
							red += palette[index].r; green += palette[index].g; blue += palette[index].b; ++count;
						}
					}
					if (count * 2 >= cells)
					{
						small->setPixel(smallX, smallY, nearestColor(palette, red / count, green / count, blue / count));
					}
				}
			}
			small->unlock();
		}
	}
	_smallRankBadges[sprite].reset(small);
	return small;
}

/**
 * Shows the soldier card under the squad button the mouse is over: the soldier's name, status,
 * weapon and the four stats with their bars. With the soldier preview option on, the card also
 * shows a live picture from the soldier's helmet camera (a small second 3D view), or TV static
 * if the soldier is dead or unconscious. The card hides when no squad button is hovered, or
 * while the mouse is captured for mouse look. With no card hovered, a camera popped up on the
 * aliens' turn (see alienTurnCamera, and alienTurnMapCameras on the map) shows the same way, in
 * 3D or on the map, under that soldier's button, with why as their status ("HIT"), looking at
 * what it pops up for (the alien shooting at them, say).
 */
void FirstPersonMode::updateSoldierCard()
{
	// The soldier under the mouse, if any (only a visible button counts, and only with a free
	// cursor).
	BattleUnit *unit = nullptr;
	int cardButton = -1;
	std::string alertStatus;
	bool poppedUp = false; // a camera popped up in the aliens' turn (its own settings), not a hovered card
	if (_hoveredSquadButton >= 0 && _hoveredSquadButton < (int)_btnSquad.size() && _btnSquad[_hoveredSquadButton]->getVisible() && !wantsMouseCapture())
	{
		unit = _squadUnits[_hoveredSquadButton];
		cardButton = _hoveredSquadButton;
	}
	else if (_alertUnit && SDL_GetTicks() < _alertUntil)
	{
		// A popped-up camera: under the soldier's squad button if it shows, else the first.
		for (size_t button = 0; button < _squadUnits.size() && button < _btnSquad.size(); ++button)
		{
			if (_squadUnits[button] == _alertUnit && _btnSquad[button]->getVisible()) cardButton = (int)button;
		}
		if (cardButton < 0 && !_btnSquad.empty()) cardButton = 0;
		unit = cardButton >= 0 ? const_cast<BattleUnit*>(_alertUnit) : nullptr;
		alertStatus = _alertText.empty() ? std::string() : std::string(_state->tr(_alertText));
		poppedUp = true;
	}
	const SquadState state = unit ? squadState(unit) : SQUAD_NONE;
	// The dead and the unconscious: their helmet camera shows only static. But for the soldier
	// looked through going down with "Quick deaths": their fall first (see aimDeathFeed).
	const bool live = unit && unit->getTile() && (state == SQUAD_READY || state == SQUAD_CONTROLLED);
	const bool deathFeed = poppedUp && unit && unit == _deathFeedUnit && SDL_GetTicks() - _deathFeedStart < DeathFeedFallTime;
	if (state == SQUAD_NONE)
	{
		unit = nullptr;
	}
	// The soldier cam (a right click on the button) shows a hovered soldier's camera big, in
	// place of the card; off the bar it closes. A fallen soldier's card stays, with its static.
	if (_hoveredSquadButton < 0 || wantsMouseCapture())
	{
		_soldierCamOpen = false;
	}
	const bool soldierCam = _soldierCamOpen && unit && live && !poppedUp;
	int soldierCamX = 0; // under the button, kept inside the canvas
	if (soldierCam)
	{
		const TextButton *button = _btnSquad[cardButton];
		soldierCamX = std::clamp(button->getX() + button->getWidth() / 2 - _soldierCamFrame->getWidth() / 2, 1, canvasWidth() - _soldierCamFrame->getWidth() - 1);
	}
	updateSoldierCam(soldierCam ? unit : nullptr, soldierCamX);
	if (soldierCam)
	{
		unit = nullptr; // (the card hides)
	}
	// Show the card's parts: the camera view only for a live soldier, the "no signal" text only
	// for a dead camera, and both only with the preview option for a hovered card (a popped-up
	// camera always shows its picture: "Aliens' turn cameras" is its own setting).
	const bool preview = unit && (Options::fpsSoldierPreview || poppedUp);
	for (const auto &part : _cardParts)
	{
		part.surface->setVisible(unit && (part.surface != _preview || (preview && (live || deathFeed))) && (part.surface != _cardSignal || (preview && !live && !deathFeed)));
	}
	if (!unit)
	{
		return;
	}
	// Place the card centred under its button, kept inside the canvas.
	const TextButton *button = _btnSquad[cardButton];
	const int cardX = std::clamp(button->getX() + button->getWidth() / 2 - CardW / 2, 1, canvasWidth() - CardW - 1);
	for (const auto &part : _cardParts)
	{
		part.surface->setX(cardX + part.dx);
		part.surface->setY(CardY + part.dy);
	}
	// Redraw the card's frame and background only when its height changes (taller with the
	// camera view). The colours come from the mod's "firstPerson" interface, element "panel",
	// if it sets them.
	const int cardHeight = preview ? 36 + PreviewH + 4 : 35;
	if (cardHeight != _cardDrawnHeight)
	{
		_cardDrawnHeight = cardHeight;
		Uint8 background, border;
		panelColors(background, border);
		_card->clear();
		_card->drawRect(0, 0, CardW, cardHeight, border);
		_card->drawRect(1, 1, CardW - 2, cardHeight - 2, background);
		if (preview)
		{
			_card->drawRect(3, 35, PreviewW + 2, PreviewH + 2, border); // frame around the camera
		}
	}
	// The text: name, weapon, and a status that says first whether the soldier has fallen or
	// been turned.
	_cardName->setText(unit->getName(_game->getLanguage(), false));
	int tagColor = -1;
	_cardWeapon->setText(weaponLabel(unit, &tagColor));
	if (tagColor >= 0) _cardWeapon->setSecondaryColor((Uint8)tagColor); // (the ammo tag, see ammoTag)
	const std::string status = state == SQUAD_DEAD ? std::string(_state->tr("STR_FPS_DECEASED"))
		: state == SQUAD_ZOMBIE ? std::string(_state->tr("STR_FPS_ZOMBIE"))
		: state == SQUAD_UNCONSCIOUS ? std::string(_state->tr("STR_FPS_UNCONSCIOUS"))
		: state == SQUAD_CONTROLLED ? std::string(_state->tr("STR_FPS_MIND_CONTROLLED"))
		: !alertStatus.empty() ? alertStatus : statusLabel(unit);
	_cardStatus->setText(status);
	// The name gives way to the status beside it (and to the rank before it).
	const bool rankShown = drawRankIcon(_cardRank, unit);
	const int nameWidth = CardW - 8 - (status.empty() ? 0 : _cardStatus->getTextWidth() + 4) - (rankShown ? RankIconW + RankIconGap : 0);
	if (nameWidth != _cardName->getWidth())
	{
		_cardName->setWidth(nameWidth);
	}
	_cardRank->setVisible(rankShown);
	_cardRank->setX(cardX + 4);
	_cardName->setX(cardX + 4 + (rankShown ? RankIconW + RankIconGap : 0)); // (the rank before the name)
	if (preview && !live && !deathFeed)
	{
		// Static on the dead camera: every pixel a random grey, new every frame (a linear
		// congruential generator, its top bits picking one of four greys).
		const SDL_Color *palette = _state->getPalette();
		const Uint8 greys[4] = { nearestColor(palette, 20, 20, 20), nearestColor(palette, 70, 70, 70),
			nearestColor(palette, 130, 130, 130), nearestColor(palette, 200, 200, 200) };
		_card->lock();
		for (int noiseY = 0; noiseY < PreviewH; ++noiseY)
		{
			for (int noiseX = 0; noiseX < PreviewW; ++noiseX)
			{
				_cardNoiseSeed = _cardNoiseSeed * 1664525u + 1013904223u;
				_card->setPixel(4 + noiseX, 36 + noiseY, greys[(_cardNoiseSeed >> 28) & 3]);
			}
		}
		_card->unlock();
	}
	// The four stats (time units, energy, health, morale) as numbers and bars, with the stun
	// level over the health bar.
	// (an overkill leaves health below zero)
	const int values[4] = { unit->getTimeUnits(), unit->getEnergy(), std::max(0, unit->getHealth()), unit->getMorale() };
	const int maxima[4] = { unit->getBaseStats()->tu, unit->getBaseStats()->stamina, unit->getBaseStats()->health, 100 };
	for (int stat = 0; stat < 4; ++stat)
	{
		_cardNumbers[stat]->setValue(values[stat]);
		_cardBars[stat]->setMax(maxima[stat]);
		_cardBars[stat]->setValue(values[stat]);
		_cardBars[stat]->setScale(statBarScale(_state, stat, maxima[stat], _cardBars[stat]->getWidth()));
	}
	_cardBars[2]->setValue2(unit->getStunlevel());

	if (preview && live)
	{
		aimHelmetCamera(_preview, unit, poppedUp ? _alertLookAt : nullptr);
	}
	else if (preview && deathFeed)
	{
		aimDeathFeed(_preview, SDL_GetTicks());
	}
}

/**
 * Points a helmet camera (the card's, the soldier cam, the map camera) through a soldier's eyes,
 * with the main 3D view's settings, and asks for a new picture.
 * @param camera The camera's view.
 * @param unit The soldier, live.
 * @param lookAt A unit to look at (a popped-up camera's alien, say); null: the way the soldier
 *   faces (or, for the soldier the 3D view looks through, the way the view looks).
 * @param lookAtPoint Or a point to look at, in voxels (an aimed action's target tile).
 */
void FirstPersonMode::aimHelmetCamera(FirstPersonView *camera, BattleUnit *unit, const BattleUnit *lookAt, const double *lookAtPoint)
{
	// When it's the soldier we're looking through, copy the 3D camera itself (its eye, and its
	// yaw by turning the camera by the difference), level.
	camera->setCameraUnit(unit);
	if (isActive() && unit == _trackedUnit)
	{
		double eye[3];
		_view->getEye(eye);
		camera->setEye(eye[0], eye[1], eye[2]);
		camera->lookAlong(unit->getDirection());
		camera->rotate(_view->getYawDegrees() - camera->getYawDegrees(), 0.0);
	}
	else
	{
		// Anyone else: an eye in the middle of the soldier's tile (or 2x2 tiles for a large unit)
		// at their eye height, looking the way they face. Voxels: a tile is 16 across.
		const Position position = unit->getPosition();
		const int unitSize = unit->getArmor()->getSize();
		camera->setEye(position.x * 16.0 + 8.0 * unitSize, position.y * 16.0 + 8.0 * unitSize, eyeHeight(unit));
		camera->lookAlong(unit->getDirection());
	}
	// At the unit to look at, if it has a place on the map (its chest, as the view turns to one).
	double point[3];
	if (lookAt && lookAt != unit && chestPoint(lookAt, point))
	{
		camera->lookAt(point);
	}
	else if (lookAtPoint)
	{
		camera->lookAt(lookAtPoint);
	}
	// The same view settings as the main 3D view, and night vision if the soldier has it (and no
	// colour or static left from a fall shown on the card, see aimDeathFeed).
	camera->setTint(0.0f);
	camera->setStatic(0.0f);
	camera->setFieldOfView(Options::fpsFieldOfView);
	camera->setSmoothLighting(Options::fpsSmoothLighting);
	camera->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
	camera->setControlHaze(controlHaze(unit, SDL_GetTicks())); // (a unit under our mind control: purple edges)
	camera->invalidate();
}

/**
 * The soldier cam: a soldier's helmet camera enlarged (a right click on their squad button, or
 * on the map camera, see handleFreeCursor), under the top bar, titled "SOLDIER CAM: name" with a
 * red dot that blinks once a second, and a little TV static over the picture: plainly a camera's
 * feed, not the view the player plays in. Moving along the bar shows each soldier's in turn.
 * Hidden with no soldier.
 * @param unit The soldier (live), or null to hide it.
 * @param frameX Where its frame's left edge goes, in canvas pixels (under the soldier's squad
 *   button, or in the map camera's place).
 */
void FirstPersonMode::updateSoldierCam(BattleUnit *unit, int frameX)
{
	const bool show = unit != nullptr;
	for (Surface *part : { (Surface*)_soldierCamFrame, (Surface*)_soldierCam, (Surface*)_txtSoldierCam })
	{
		part->setVisible(show);
	}
	_soldierCamDot->setVisible(show && SDL_GetTicks() % 1000 < 600);
	_soldierCamRank->setVisible(false); // (shown below, for a soldier with a rank)
	if (!show)
	{
		return;
	}

	_soldierCamFrame->setX(frameX);
	_soldierCamFrame->setY(CardY);
	_soldierCam->setX(frameX + 4);
	_soldierCam->setY(CardY + 16);
	_soldierCamDot->setX(frameX + 5);
	_soldierCamDot->setY(CardY + 5);
	// The soldier's rank before the title (after the red dot), the title after it.
	const bool camRank = drawRankIcon(_soldierCamRank, unit);
	_soldierCamRank->setVisible(camRank);
	_soldierCamRank->setX(frameX + 13);
	_soldierCamRank->setY(CardY + 4 - RankIconRise);
	_txtSoldierCam->setX(frameX + 13 + (camRank ? RankIconW + RankIconGap : 0));
	_txtSoldierCam->setY(CardY + 4);
	const std::string title = _state->tr("STR_FPS_SOLDIER_CAM").arg(unit->getName(_game->getLanguage(), false));
	if (_txtSoldierCam->getText() != title)
	{
		_txtSoldierCam->setText(title);
	}
	aimHelmetCamera(_soldierCam, unit);
}

/**
 * The map camera (setting "Map camera"): on the isometric map in the player's turn, the selected
 * soldier's helmet camera in a window at the top left under the top bar (right of OXCE's touch
 * buttons when they show), titled with their name. It looks the way they face or, while an
 * action is being aimed on the map, at the tile under the map's cursor (at the unit there, if
 * it's seen): the target through the soldier's eyes. It's made again when the setting's size
 * or the canvas's changes. The soldier cam opened from it (a right click, see handleFreeCursor) shows the same
 * soldier big in its place until the cursor leaves it or another right click; a squad button's
 * soldier cam wins over it. Hidden in 3D, in the aliens' turn, and with none of the player's
 * units selected.
 */
void FirstPersonMode::updateMapCamera()
{
	BattleUnit *unit = _save->getSelectedUnit();
	const bool show = Options::fpsMapCamera > 0 && !isActive() && _save->getSide() == FACTION_PLAYER
		&& unit && unit->getFaction() == FACTION_PLAYER && unit->getTile();
	if (show && (Options::fpsMapCamera != _mapCameraSize || canvasWidth() != _mapCameraCanvasWidth))
	{
		makeMapCamera(Options::fpsMapCamera);
	}

	// Its place: at the left under the top bar, or right of the touch buttons (screen pixels).
	const int frameX = _state->_btnCtrl->getVisible() ? (MapCameraTouchClear + _uiScale - 1) / _uiScale : 1;
	_mapCameraFrame->setX(frameX);
	_mapCameraFrame->setY(CardY);
	_mapCamera->setX(frameX + MapCameraBorder);
	_mapCamera->setY(CardY + MapCameraBorder + MapCameraTitleH);
	_mapCameraDot->setX(frameX + 4);
	_mapCameraDot->setY(CardY + 4);
	_txtMapCamera->setX(frameX + 12);
	_txtMapCamera->setY(CardY + 2);

	// The soldier cam opened from it closes when the map camera goes, when a squad button's
	// soldier cam shows (see updateSoldierCard, just before), or when the cursor leaves it.
	const bool squadSoldierCam = _soldierCamFrame->getVisible();
	const bool cursorOnSoldierCam = _cursorX >= frameX && _cursorX < frameX + _soldierCamFrame->getWidth()
		&& _cursorY >= CardY && _cursorY < CardY + _soldierCamFrame->getHeight();
	if (!show || squadSoldierCam || !cursorOnSoldierCam)
	{
		_mapSoldierCamOpen = false;
	}
	if (_mapSoldierCamOpen)
	{
		updateSoldierCam(unit, frameX);
	}

	const bool showWindow = show && !_mapSoldierCamOpen;
	for (Surface *part : { (Surface*)_mapCameraFrame, (Surface*)_mapCamera, (Surface*)_txtMapCamera })
	{
		part->setVisible(showWindow);
	}
	_mapCameraDot->setVisible(showWindow && SDL_GetTicks() % 1000 < 600); // (blinking, as the soldier cam's)
	if (!showWindow)
	{
		return;
	}
	const std::string title = unit->getName(_game->getLanguage(), false);
	if (_txtMapCamera->getText() != title)
	{
		_txtMapCamera->setText(title);
	}

	// Where it looks: while an action is aimed, at the tile under the map's cursor (not the
	// soldier's own): the unit there if it's seen, else a little over the tile's floor.
	const BattleUnit *lookAtUnit = nullptr;
	double tilePoint[3];
	const double *lookAtPoint = nullptr;
	if (_state->_battleGame->getCurrentAction()->targeting)
	{
		Position target;
		_state->_map->getSelectorPosition(&target);
		const Tile *tile = _save->getTile(target);
		if (tile && target != unit->getPosition())
		{
			const BattleUnit *there = tile->getUnit();
			if (there && there != unit && there->getVisible())
			{
				lookAtUnit = there;
			}
			else
			{
				tilePoint[0] = target.x * 16.0 + 8.0;
				tilePoint[1] = target.y * 16.0 + 8.0;
				tilePoint[2] = target.z * 24.0 - tile->getTerrainLevel() + MapCameraAimHeight;
				lookAtPoint = tilePoint;
			}
		}
	}
	aimHelmetCamera(_mapCamera, unit, lookAtUnit, lookAtPoint);
}

/**
 * Is a point on the canvas on the map camera (as shown), or on the soldier cam opened from it?
 * There it's the map camera's (see handleFreeCursor), not the map's.
 * @param canvasX The point, in canvas pixels.
 * @param canvasY The same, down.
 * @return True if it is.
 */
bool FirstPersonMode::onMapCamera(int canvasX, int canvasY) const
{
	auto inside = [canvasX, canvasY](const Surface *surface)
	{
		return surface && surface->getVisible() && canvasX >= surface->getX() && canvasX < surface->getX() + surface->getWidth()
			&& canvasY >= surface->getY() && canvasY < surface->getY() + surface->getHeight();
	};
	return inside(_mapCameraFrame) || (_mapSoldierCamOpen && inside(_soldierCamFrame));
}

/**
 * Keeps the banner across the top of the 3D view up to date. It says, in order of importance:
 * BURNING for a moment after the soldier catches fire, ENEMY SPOTTED for a moment after a new
 * enemy comes into sight in the player's turn, or HIDDEN MOVEMENT during the aliens' turn while
 * no enemy is in sight (as the isometric map does). The letters glow and pulse; the alarms
 * (burning, enemy spotted) pulse faster and in red.
 * @param playerTurn True in the player's turn.
 */
void FirstPersonMode::updateBanner(bool playerTurn)
{
	// Pick the text: the first that applies, or none.
	std::string text;
	const bool burning = isActive() && SDL_GetTicks() < _burningBannerUntil;
	if (burning)
	{
		text = _state->tr("STR_FPS_BURNING");
	}
	else if (isActive() && playerTurn && SDL_GetTicks() < _spottedUntil)
	{
		text = _state->tr("STR_FPS_ENEMY_SPOTTED");
	}
	else if (isActive() && !playerTurn)
	{
		// Hidden movement, unless an enemy can be seen (then the view itself shows them).
		text = _state->tr("STR_HIDDEN_MOVEMENT");
		for (BattleUnit *unit : *_save->getUnits())
		{
			if (!unit->isOut() && unit->getFaction() == FACTION_HOSTILE && unit->getVisible())
			{
				text.clear();
				break;
			}
		}
	}
	// Set the text on the banner and on the glow copies around it, only when it changed.
	if (!text.empty() && _txtBanner->getText() != text)
	{
		_txtBanner->setText(text);
		for (Text *glow : _bannerGlow)
		{
			glow->setText(text);
		}
	}
	// Show the banner and its glow only with something to say.
	_txtBanner->setVisible(!text.empty());
	for (Text *glow : _bannerGlow)
	{
		glow->setVisible(!text.empty());
	}
	if (text.empty())
	{
		return;
	}
	// Glowing letters: a yellow core that pulses towards orange, in a halo going red to purple
	// (enemy spotted and burning pulse faster, in plain red; on the civilians' turn the halo goes
	// between greens, as the map's turn bar turns green then: a player's report, GitHub issue #55).
	// The pulse is a sine of the time: one radian every 160 ms for an alarm, every 450 ms otherwise.
	const SDL_Color *palette = _game->getScreen()->getPalette();
	const bool alarm = playerTurn || burning;
	const double pulsePhase = SDL_GetTicks() / (alarm ? 160.0 : 450.0);
	const double pulse = 0.5 + 0.5 * std::sin(pulsePhase);
	const Uint8 core = pulse > 0.5 ? textColor(palette, 255, 230, 70) : textColor(palette, 255, 170, 40);
	const bool civiliansTurn = !playerTurn && _save->getSide() == FACTION_NEUTRAL;
	const Uint8 halo = alarm ? textColor(palette, 240, 40, 30)
		: civiliansTurn ? (std::sin(pulsePhase * 0.37) > 0 ? textColor(palette, 60, 210, 70) : textColor(palette, 40, 160, 120))
		: (std::sin(pulsePhase * 0.37) > 0 ? textColor(palette, 235, 50, 40) : textColor(palette, 190, 70, 230));
	// Recolour only on change (setting a colour redraws the text).
	if (_txtBanner->getColor() != core)
	{
		_txtBanner->setColor(core);
	}
	for (Text *glow : _bannerGlow)
	{
		if (glow->getColor() != halo) glow->setColor(halo);
	}
	if (_textShadows.front()->getColor() != halo) _textShadows.front()->setColor(halo); // the banner's shadow is part of the halo
}

/**
 * Works out which unit each squad button stands for: the player's own units in a stable order
 * (so the numbers don't shift when someone falls), then any aliens under the player's mind
 * control. Makes more buttons (each with an icon surface) when the squad has grown, and updates
 * each button's look when its soldier's state changed: a fallen or turned soldier's button
 * shows an icon instead of the number, in a colour of its own. Lays the bar out again if
 * anything changed.
 */
void FirstPersonMode::refreshSquad()
{
	// Everyone who came with the player (the fallen too, so their button keeps its place),
	// then the aliens the player controls right now.
	std::vector<BattleUnit*> squad;
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getOriginalFaction() == FACTION_PLAYER)
		{
			squad.push_back(unit);
		}
	}
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getOriginalFaction() != FACTION_PLAYER && unit->getFaction() == FACTION_PLAYER && !unit->isOut())
		{
			squad.push_back(unit);
		}
	}
	// Make a button, an icon and a state slot for every unit beyond the buttons there are
	// (buttons are never removed; spare ones stay hidden).
	while (_btnSquad.size() < squad.size())
	{
		TextButton *button = makeButton(SquadX, 16, std::to_string(_btnSquad.size() + 1));
		button->setGroup(&_squadButtonGroup);
		button->setVisible(false);
		_squadColor = button->getColor();
		_btnSquad.push_back(button);
		Surface *icon = new Surface(7, 8, 0, BarY + 2);
		_state->add(icon);
		icon->setVisible(false);
		_ownSurfaces.insert(icon);
		_topBarSurfaces.insert(icon);
		_squadIcons.push_back(icon);
		_squadStates.push_back(SQUAD_NONE);
	}
	// One unit per button (null for the spare ones).
	squad.resize(_btnSquad.size(), nullptr);
	bool changed = squad != _squadUnits;
	_squadUnits = squad;
	// How each one is: a fallen or turned soldier's button shows an icon instead of the number.
	// A button's colour is the base of a 16-colour group (TextButton shades from it), so pick
	// the group whose fourth shade comes nearest to the wanted colour (weighted distance, green
	// counting most, as the eye sees it).
	const SDL_Color *palette = _state->getPalette();
	auto colorGroupNear = [palette](int red, int green, int blue) {
		int best = 0, bestDist = INT_MAX;
		for (int group = 0; group < 16; ++group)
		{
			const SDL_Color &sample = palette[group * 16 + 3];
			const int dist = (sample.r - red) * (sample.r - red) * 3 + (sample.g - green) * (sample.g - green) * 6 + (sample.b - blue) * (sample.b - blue);
			if (dist < bestDist) { bestDist = dist; best = group; }
		}
		return (Uint8)(best * 16);
	};
	// Restyle the buttons whose soldier's state changed: number or icon, and the colour
	// (orange: mind controlled, grey: dead, blue: unconscious, green: zombie; purple: a unit of the
	// other side under our mind control, as the haze at the view's edges, see controlHaze).
	for (size_t slot = 0; slot < _squadUnits.size(); ++slot)
	{
		const SquadState state = squadState(_squadUnits[slot]);
		if (state == _squadStates[slot]) continue;
		_squadStates[slot] = state;
		changed = true;
		TextButton *button = _btnSquad[slot];
		button->setText(state == SQUAD_READY ? std::to_string(slot + 1) : std::string());
		const bool ourControl = state == SQUAD_READY && _squadUnits[slot]->getOriginalFaction() != FACTION_PLAYER;
		button->setColor(state == SQUAD_CONTROLLED ? colorGroupNear(230, 130, 30) : state == SQUAD_DEAD ? colorGroupNear(120, 120, 120)
			: state == SQUAD_UNCONSCIOUS ? colorGroupNear(90, 120, 170) : state == SQUAD_ZOMBIE ? colorGroupNear(90, 140, 60)
			: ourControl ? colorGroupNear(170, 80, 220) : _squadColor);
		drawSquadIcon(_squadIcons[slot], state);
	}
	if (changed)
	{
		layoutTopBar();
	}
}

/**
 * Lays out the right part of the top bar (canvas pixels). From the right: END, then the
 * armor's buttons (with long labels, or short ones if the bar is too narrow for them), then
 * the squad buttons between the previous and next buttons. The squad buttons take what room
 * is left: ten are shown at once (a window that scrolls over the rest), or more on a screen with
 * room for them at their full width (the owner: "If screen permits we can have more soldier
 * numbers up"), each at least SquadMinW and at most SquadFullW pixels wide. Each button keeps
 * its slot's number. With room to
 * spare (a wide screen, a small squad) the squad's group, previous to next, stands in the
 * middle of the screen (the owner's ask: the left buttons at the left edge, the soldiers in the
 * middle, the right ones at the right edge), moved aside only as far as it must to clear its
 * neighbours; on the original screen a full squad fills the bar as before.
 */
void FirstPersonMode::layoutTopBar()
{
	// The slots with a button: every soldier we came with (the fallen too), and aliens under
	// our control. Hide all the buttons for now; the ones in the window are shown below.
	_squadSlots.clear();
	for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
	{
		const SquadState state = buttonIndex < _squadStates.size() ? _squadStates[buttonIndex] : SQUAD_NONE;
		_btnSquad[buttonIndex]->setVisible(false);
		_squadIcons[buttonIndex]->setVisible(false);
		if (state != SQUAD_NONE) _squadSlots.push_back(buttonIndex);
	}
	const int wantedButtons = std::min<int>(SquadWindow, (int)_squadSlots.size());
	// The suit's buttons, right to left from END. A first pass tries the long labels; if that
	// leaves too little room for the squad buttons, a second pass uses the short ones.
	// freeRight ends up as the right end of the room left for the squad buttons.
	TextButton *suitButtons[4] = { _btnFly, _btnLight, _btnNight, _btnSpecial };
	// Previous's leftmost place: after INV, wherever INV stands (in HUD's place on the map, see
	// updateTopBar).
	const int squadX = SquadX - (InventoryButtonX - _btnInventory->getX());
	const int squadLeft = squadX + PrevNextW + 1;
	int freeRight = 0;
	for (int pass = 0; pass < 2; ++pass)
	{
		freeRight = canvasWidth() - 1 - EndTurnW - 2;
		for (int suitIndex = 0; suitIndex < 4; ++suitIndex)
		{
			if (!suitButtons[suitIndex]->getVisible()) continue;
			const std::string &label = pass == 0 ? _suitLabelsLong[suitIndex] : _suitLabelsShort[suitIndex];
			suitButtons[suitIndex]->setText(label);
			suitButtons[suitIndex]->setWidth(labelWidth(label));
			freeRight -= suitButtons[suitIndex]->getWidth();
			suitButtons[suitIndex]->setX(freeRight);
			freeRight -= 1;
		}
		if (freeRight - 1 - PrevNextW - squadLeft >= wantedButtons * (SquadMinW + 1)) break;
	}
	// Ten squad buttons between previous and next (as many as fit at the least width), or more
	// if they fit at the full width; the window scrolls over the rest. Work out how many, keep the
	// scroll position inside the slots, and share out the width.
	const int availableWidth = freeRight - 1 - PrevNextW - squadLeft;
	_squadWindow = std::clamp(availableWidth / (SquadMinW + 1), 1, std::max(1, wantedButtons));
	_squadWindow = std::max(_squadWindow, std::min((int)_squadSlots.size(), availableWidth / (SquadFullW + 1)));
	_squadScroll = std::clamp(_squadScroll, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	const int buttonWidth = std::clamp(availableWidth / _squadWindow - 1, SquadMinW, SquadFullW);
	// The group's place: centred on the screen, within the room it has (its spare width).
	const int shownButtons = std::min((int)_squadSlots.size() - _squadScroll, _squadWindow);
	const int groupWidth = PrevNextW + 1 + shownButtons * (buttonWidth + 1) + PrevNextW;
	const int spareWidth = std::max(0, availableWidth - shownButtons * (buttonWidth + 1));
	const int groupX = squadX + std::clamp((canvasWidth() - groupWidth) / 2 - squadX, 0, spareWidth);
	// Place previous, the buttons in the window (each with its icon centred on it, shown for a
	// soldier who isn't ready), and next right after them.
	_btnPrev->setX(groupX);
	int buttonX = groupX + PrevNextW + 1;
	for (int slotIndex = _squadScroll; slotIndex < std::min((int)_squadSlots.size(), _squadScroll + _squadWindow); ++slotIndex)
	{
		const size_t buttonIndex = _squadSlots[slotIndex];
		_btnSquad[buttonIndex]->setX(buttonX);
		_btnSquad[buttonIndex]->setWidth(buttonWidth);
		_btnSquad[buttonIndex]->setVisible(true);
		_squadIcons[buttonIndex]->setX(buttonX + (buttonWidth - 7) / 2);
		_squadIcons[buttonIndex]->setVisible(_squadStates[buttonIndex] != SQUAD_READY);
		buttonX += buttonWidth + 1;
	}
	_btnNext->setX(buttonX);
}

/**
 * Scrolls the window of squad buttons by some slots (the mouse wheel over the bar, or following
 * the selected soldier), kept within the slots there are.
 * @param step Slots to scroll: positive towards the later slots, negative towards the first.
 */
void FirstPersonMode::scrollSquad(int step)
{
	const int newScroll = std::clamp(_squadScroll + step, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	if (newScroll != _squadScroll)
	{
		_squadScroll = newScroll;
		layoutTopBar();
	}
}

/**
 * The mouse wheel over a soldier's button: moves the soldier card (and the soldier cam, if
 * open) on to the next or previous soldier in the squad's order, round from the last to the
 * first, the dead and the unconscious included, scrolling the window of buttons to keep theirs
 * in it. The cursor stays where it is; moving it onto another button shows that one's soldier.
 * @param step 1 for the next soldier, -1 for the previous.
 * @param cursorX Where the cursor is across the canvas: over a squad button (where to start if no card shows).
 * @param cursorY Where the cursor is down the canvas.
 */
void FirstPersonMode::browseSquad(int step, double cursorX, double cursorY)
{
	// The slot whose card shows now (else the one under the cursor), and the slot a step on,
	// round the ends.
	const TextButton *buttonUnderCursor = buttonAt(cursorX, cursorY);
	const int slotCount = (int)_squadSlots.size();
	int shownSlot = -1, cursorSlot = -1;
	for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
	{
		if ((int)_squadSlots[slotIndex] == _hoveredSquadButton) shownSlot = slotIndex;
		if (_btnSquad[_squadSlots[slotIndex]] == buttonUnderCursor) cursorSlot = slotIndex;
	}
	const int fromSlot = shownSlot >= 0 ? shownSlot : cursorSlot;
	if (fromSlot < 0)
	{
		return;
	}
	const int nextSlot = (fromSlot + step % slotCount + slotCount) % slotCount;

	// Bring that soldier's button into the window and show their card.
	if (nextSlot < _squadScroll) scrollSquad(nextSlot - _squadScroll);
	else if (nextSlot >= _squadScroll + _squadWindow) scrollSquad(nextSlot - _squadScroll - _squadWindow + 1);
	_hoveredSquadButton = (int)_squadSlots[nextSlot];

	// The button now under the cursor (the window may have scrolled under it): the card moves
	// back only when the cursor comes onto another one (see trackHover).
	_buttonUnderCursor = buttonAt(cursorX, cursorY);
}

/**
 * Tells how a squad member is: ready, dead, a zombie (killed and turned into one), unconscious,
 * or turned against us by the aliens' mind control. An alien we control is ready while that
 * lasts, and has no button otherwise.
 * @param unit The unit of a squad button, or null.
 * @return Its state; SQUAD_NONE means it gets no button.
 */
FirstPersonMode::SquadState FirstPersonMode::squadState(const BattleUnit *unit) const
{
	// No unit, or one not in this battle.
	if (!unit || unit->isIgnored())
	{
		return SQUAD_NONE; // (left behind in an earlier stage of the mission: not in this battle)
	}
	// An alien: on the squad bar only while under our control and standing.
	if (unit->getOriginalFaction() != FACTION_PLAYER)
	{
		return unit->getFaction() == FACTION_PLAYER && !unit->isOut() ? SQUAD_READY : SQUAD_NONE;
	}
	// Our own soldier, dead (killed outright, or down with no health left).
	if (unit->getStatus() == STATUS_DEAD || (unit->isOut() && unit->getHealth() <= 0))
	{
		return unit->getAlreadyRespawned() ? SQUAD_ZOMBIE : SQUAD_DEAD; // (turned into a zombie when killed)
	}
	// Down but alive: unconscious.
	if (unit->isOut())
	{
		return SQUAD_UNCONSCIOUS;
	}
	// Standing: ours, or the aliens' while under their mind control.
	return unit->getFaction() == FACTION_PLAYER ? SQUAD_READY : SQUAD_CONTROLLED;
}

/**
 * Draws the pixel-art icon (7x8 pixels) that replaces a squad button's number: a skull for the
 * dead (bone white, rotting green for a zombie), a caduceus for the unconscious (a gold staff
 * with green snakes), and an alien face for a soldier under the aliens' mind control (grey
 * skin, black eyes). Other states leave the icon empty.
 * @param icon The icon surface to draw on.
 * @param state The squad member's state.
 */
void FirstPersonMode::drawSquadIcon(Surface *icon, SquadState state)
{
	// The pictures, one string per row: '#' the main ink, 'o' the second ink, '.' transparent.
	static const char *skull[8] = { ".#####.", "#######", "#..#..#", "#..#..#", "###.###", ".#####.", ".#.#.#.", "..###.." };
	static const char *caduceus[8] = { "##.#.##", ".#####.", "...#...", ".##o##.", "o..#..o", ".oo#oo.", "...#...", "..###.." };
	static const char *alien[8] = { "..###..", ".#####.", "#######", "#oo#oo#", "#.o#o.#", ".#####.", "..###..", "...#..." };
	// Pick the picture and its two inks for the state, as the palette's nearest colours.
	const SDL_Color *palette = _state->getPalette();
	const char **art = state == SQUAD_DEAD || state == SQUAD_ZOMBIE ? skull
		: state == SQUAD_UNCONSCIOUS ? caduceus : state == SQUAD_CONTROLLED ? alien : nullptr;
	Uint8 ink = 0, secondInk = 0;
	switch (state)
	{
	case SQUAD_DEAD: ink = nearestColor(palette, 235, 230, 205); break;                                    // bone
	case SQUAD_ZOMBIE: ink = nearestColor(palette, 150, 210, 90); break;                                   // rotting green
	case SQUAD_UNCONSCIOUS: ink = nearestColor(palette, 245, 205, 70); secondInk = nearestColor(palette, 120, 230, 140); break; // gold staff, green snakes
	case SQUAD_CONTROLLED: ink = nearestColor(palette, 180, 200, 175); secondInk = nearestColor(palette, 10, 10, 10); break;   // grey skin, black eyes
	default: break;
	}
	icon->clear();
	if (!art)
	{
		return;
	}
	// Copy the picture onto the icon, pixel by pixel.
	icon->lock();
	for (int iconY = 0; iconY < 8; ++iconY)
	{
		for (int iconX = 0; iconX < 7; ++iconX)
		{
			if (art[iconY][iconX] == '#') icon->setPixel(iconX, iconY, ink);
			else if (art[iconY][iconX] == 'o') icon->setPixel(iconX, iconY, secondInk);
		}
	}
	icon->unlock();
}

/**
 * Keeps the top bar in sync with the game every frame: the squad buttons (see refreshSquad),
 * the window of squad buttons scrolled to show the selected soldier, the selected soldier's
 * button pressed, the HUD button pressed while the classic HUD is on, the HUD and END buttons
 * only in the 3D view, and the view button saying where a click takes you (Iso or 3D).
 * @param selected The selected unit, or null.
 */
void FirstPersonMode::updateTopBar(BattleUnit *selected)
{
	refreshSquad();
	// The window of squad buttons follows the selected soldier: when another soldier is
	// selected, scroll just far enough to bring their button into the window.
	if (selected != _squadScrolledTo)
	{
		_squadScrolledTo = selected;
		for (int slotIndex = 0; slotIndex < (int)_squadSlots.size(); ++slotIndex)
		{
			if (_squadUnits[_squadSlots[slotIndex]] != selected) continue;
			if (slotIndex < _squadScroll) scrollSquad(slotIndex - _squadScroll);
			else if (slotIndex >= _squadScroll + _squadWindow) scrollSquad(slotIndex - _squadScroll - _squadWindow + 1);
		}
	}

	// The selected soldier's button shows pressed (the group pointer points at it); redraw the
	// buttons when that changes.
	TextButton *squadGroup = nullptr;
	for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
	{
		if (_squadUnits[buttonIndex] && _squadUnits[buttonIndex] == selected)
		{
			squadGroup = _btnSquad[buttonIndex];
		}
	}
	if (squadGroup != _squadButtonGroup)
	{
		_squadButtonGroup = squadGroup;
		for (auto *button : _btnSquad)
		{
			button->invalidate();
		}
	}

	// The HUD button: pressed while the classic HUD is drawn, shown only in 3D (on the map INV
	// takes its place, no gap left, and the bar is laid out again). END too shows only in 3D,
	// and the view button names the view a click switches to.
	const bool active = isActive();
	const int inventoryX = active ? InventoryButtonX : HudButtonX;
	if (_btnInventory->getX() != inventoryX)
	{
		_btnInventory->setX(inventoryX);
		layoutTopBar();
	}
	TextButton *hudGroup = _classicHud ? _btnHud : nullptr;
	if (hudGroup != _hudButtonGroup)
	{
		_hudButtonGroup = hudGroup;
		_btnHud->invalidate();
	}
	_btnHud->setVisible(active);
	_btnEndTurn->setVisible(active); // the classic HUD has its own
	_btnView->setText(_state->tr(active ? "STR_FPS_BUTTON_ISO" : "STR_FPS_BUTTON_3D"));
}

/**
 * Finds which of our top bar buttons is under a point, if any. Only visible buttons count.
 * @param x The point's x, in canvas pixels.
 * @param y The point's y, in canvas pixels.
 * @return The button under the point, or null.
 */
TextButton *FirstPersonMode::buttonAt(double x, double y) const
{
	// With the 3D HUD hidden there's no top bar to click (nor hover, so no soldier card).
	if (hudHidden())
	{
		return nullptr;
	}
	// Is the point inside this button's rectangle (and is the button showing)?
	auto inside = [&](const TextButton *button) {
		return button->getVisible() && x >= button->getX() && x < button->getX() + button->getWidth()
			&& y >= button->getY() && y < button->getY() + button->getHeight();
	};
	// The fixed buttons first, then the squad buttons.
	for (TextButton *button : { _btnView, _btnHud, _btnInventory, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnPrev, _btnNext })
	{
		if (inside(button)) return button;
	}
	for (TextButton *button : _btnSquad)
	{
		if (inside(button)) return button;
	}
	return nullptr;
}

/**
 * Acts on a click on one of our top bar buttons. Most hand the click to the battlescape's own
 * button of the same job (inventory, end turn, personal light, special weapon, previous and
 * next soldier), so the game's rules for it apply; the view, HUD, flight and night vision
 * buttons are ours, and a squad button selects its soldier.
 * @param button The button clicked.
 * @param action The click, handed on to the battlescape's handlers.
 */
void FirstPersonMode::buttonClicked(TextButton *button, Action *action)
{
	// One branch per button (the squad buttons last).
	if (button == _btnView)
	{
		toggleView();
	}
	else if (button == _btnHud)
	{
		setClassicHud(!_classicHud);
	}
	else if (button == _btnInventory)
	{
		_state->btnInventoryClick(action);
	}
	else if (button == _btnEndTurn)
	{
		_state->btnEndTurnClick(action);
	}
	else if (button == _btnFly)
	{
		toggleFlight(_save->getSelectedUnit());
	}
	else if (button == _btnLight)
	{
		_state->btnPersonalLightingClick(action);
	}
	else if (button == _btnNight)
	{
		toggleNightVision(_trackedUnit);
	}
	else if (button == _btnSpecial)
	{
		_state->btnSpecialClick(action);
	}
	else if (button == _btnPrev)
	{
		_state->btnPrevSoldierClick(action);
	}
	else if (button == _btnNext)
	{
		_state->btnNextSoldierClick(action);
	}
	else
	{
		// A squad button: select the soldier it stands for (buttons of empty slots do nothing).
		for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
		{
			if (button == _btnSquad[buttonIndex] && _squadUnits[buttonIndex])
			{
				selectUnit(_squadUnits[buttonIndex]);
			}
		}
	}
}

/**
 * Selects a soldier from the squad bar, the same way the battlescape's next-soldier button
 * does: the isometric camera centres on them, any action in progress is cancelled, and the
 * cursor is set up for the new soldier. Nothing happens while the battlescape's buttons are
 * locked (the aliens' turn, an action running, a shot in flight, a panic not yet dealt with),
 * or for a soldier who is down or not under the player's control.
 * @param unit The soldier to select.
 */
void FirstPersonMode::selectUnit(BattleUnit *unit)
{
	// Only a soldier the player can command, and only when the game takes orders.
	if (!_state->allowButtons() || unit->isOut() || unit->getFaction() != FACTION_PLAYER)
	{
		return;
	}
	// Select them, as BattlescapeState's next-soldier button does.
	_save->setSelectedUnit(unit);
	_state->updateSoldierInfo();
	_state->_map->getCamera()->centerOnPosition(unit->getPosition());
	_state->_battleGame->cancelAllActions();
	_state->_battleGame->getCurrentAction()->actor = unit;
	_state->_battleGame->setupCursor();
}

/**
 * Hands the 3D view the visible enemies with their numbers, so it marks them. The numbers are
 * those of the classic HUD's red buttons on the right (one per enemy in sight), so a marker in
 * the 3D view and a button on the classic HUD always agree.
 */
void FirstPersonMode::updateEnemyMarkers()
{
	// Every red button showing, with a standing enemy behind it, numbered from 1.
	std::vector<std::pair<const BattleUnit*, int>> markers;
	for (int buttonIndex = 0; buttonIndex < BattlescapeState::VISIBLE_MAX; ++buttonIndex)
	{
		BattleUnit *enemy = _state->_visibleUnit[buttonIndex];
		if (enemy && !enemy->isOut() && _state->_btnVisibleUnit[buttonIndex]->getVisible() && !hudHidden())
		{
			markers.push_back({ enemy, buttonIndex + 1 });
		}
	}
	_view->setMarkers(markers);
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
