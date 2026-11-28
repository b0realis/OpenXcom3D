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
 * FirstPersonMode.cpp: the heart of the first-person mode.
 *
 * Creating the mode (the 3D view, the top bar, the panel, the crosshair, the soldier card),
 * laying our surfaces out on a canvas that is magnified on big screens, switching between the
 * isometric map and the 3D view, the per-frame update (think), input (handle) and drawing the
 * screen (blit).
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include "FirstPersonAmbience.h"
#include "FirstPersonApi.h"
#include "FirstPersonScanner.h"
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "FirstPersonVoxelBodies.h"
#include "FirstPersonVoxelUnits.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/ExplosionBState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Explosion.h"
#include "../Battlescape/Projectile.h"
#include <fstream>
#include <sstream>
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitSprite.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/Timer.h"
#include "../Interface/Bar.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Mod.h"
#include "../Engine/Sound.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/SavedGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/// 3DIso's flight's length (setting "3DIso flight": Quick or Smooth; research).
static double isoFlightSeconds() { return Options::fpsIsoFlight == 1 ? 0.7 : 1.5; }

/**
 * 3DIso (research): a move on its picture (in the map's screen pixels) as the move on the map's own picture that shows the
 * same ground: as fast on the screen at any zoom, and the way the picture shows it turned or tilted. The ground's step on the
 * screen, across and down, for the camera's yaw and pitch (M), turned back into ground, and that ground's step on the map's
 * own picture (M0).
 * @param turn, tilt 3DIso's turn and tilt now, degrees.
 * @param zoom Its zoom.
 * @param x, y The move; set to the map's.
 */
static void isoMoveOnMap(double turn, double tilt, double zoom, double &x, double &y)
{
	const double Degree = 3.14159265358979323846 / 180.0, yaw = (-135.0 + turn) * Degree, pitch = std::clamp(-30.0 - tilt, -80.0, -10.0) * Degree;
	const double yaw0 = -135.0 * Degree, pitch0 = -30.0 * Degree;
	const double m[2][2] = { { -std::sin(yaw), std::cos(yaw) }, { std::sin(pitch) * std::cos(yaw), std::sin(pitch) * std::sin(yaw) } };
	const double m0[2][2] = { { -std::sin(yaw0), std::cos(yaw0) }, { std::sin(pitch0) * std::cos(yaw0), std::sin(pitch0) * std::sin(yaw0) } };
	const double determinant = m[0][0] * m[1][1] - m[0][1] * m[1][0];
	if (std::abs(determinant) < 1e-9 || zoom <= 0.0) return;
	const double screenX = x / zoom, screenY = y / zoom;
	const double groundX = (m[1][1] * screenX - m[0][1] * screenY) / determinant, groundY = (-m[1][0] * screenX + m[0][0] * screenY) / determinant;
	x = m0[0][0] * groundX + m0[0][1] * groundY;
	y = m0[1][0] * groundX + m0[1][1] * groundY;
}

namespace
{

// The game remakes the battlescape screen (and with it this mode) when options change
// mid-battle; what the player had chosen (3D or not, the classic HUD, the flashlight, the ways
// of walking) carries over to the new screen of the same battle. Loading a game from inside a
// battle remakes the screen too, with a new battle object: that carries over as well, when the
// old screen ran only a moment ago.

/// The battle the choices below were made in.
const SavedBattleGame *lastBattle = nullptr;
/// Whether the player had switched to the 3D view (FirstPersonMode::_wants3D).
bool lastWants3D = false;
/// Whether the classic HUD was shown over the 3D view (FirstPersonMode::_classicHud).
bool lastClassicHud = false;
/// Whether the 3D HUD was hidden (FirstPersonMode::_hudHidden), and the crosshair too (_hudClean).
bool lastHudHidden = false, lastHudClean = false;
/// Whether the flashlight was switched on (FirstPersonMode::_flashlightOn).
bool lastFlashlightOn = true;
/// Whether sprinting and en passant were switched on (FirstPersonMode::_sprintOn, _enPassantOn).
bool lastSprintOn = false, lastEnPassantOn = false;
/// The cheats the doom mode key would switch back on (FirstPersonMode::_pausedCheats).
int lastPausedCheats = 0;
/// The Doom real-time fuses burning (FirstPersonMode::_doomFuses): only for the same battle.
std::unordered_map<int, FirstPersonMode::DoomFuse> lastDoomFuses;
/// When a first-person mode last ran its per-frame update (SDL ticks, ms; 0: never).
Uint32 lastThinkTicks = 0;
/// The map camera's size the 3D HUD key brings back on the map (see toggleMapCamera): the last
/// one it had, medium at first.
int lastMapCameraSize = 2;


}

/**
 * Creates the first-person mode: the 3D view and all our own surfaces (crosshair, banner, lift
 * arrows, top bar, info panel, soldier card, weapon in hand), added to the battlescape state.
 * The view goes right above the isometric map, so the classic HUD still draws over it; the
 * crosshair, top bar and panel go on top of everything.
 * When the screen was remade for the same battle (options changed, or a game was loaded from
 * inside a battle a moment ago), the player's choices (3D or not, classic HUD) carry over.
 * @param game Pointer to the core game.
 * @param state The battlescape screen.
 */
FirstPersonMode::FirstPersonMode(Game *game, BattlescapeState *state) :
	_game(game), _state(state), _save(state->_save), _wants3D(Options::fpsStartInFirstPerson), _missionAtStart(state->_save->getMissionType()), _warningY(state->_warning->getY()),
	_gameLineX(state->_txtDebug->getX())
{
	_fullColorDisplay = FirstPerson::wantsFullColorDisplay(); // (the display was opened by the settings as they are)
	// The overlays are laid out on a canvas; on big screens it's drawn magnified (see blit).
	// Whether each step is allowed is not known yet (see stepAllowed).
	_targetWidth = Options::baseXResolution; // (the canvas goes to the display only once 3D shows: see relayoutIfNeeded)
	_targetHeight = Options::baseYResolution;
	_uiScale = uiScaleFor(_targetWidth, _targetHeight);
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
	FirstPerson::settleOptions(); // (the smoke's first value, from an older setting)
	FirstPerson::clearQuickFalls(); // (a new battle, or one loaded: none of the last one's falls)
	FirstPerson::clearAirFalls();
	FirstPerson::clearChains(); // (nor its chain of explosions' ripple)
	_voxelUnits = std::make_unique<VoxelUnits>(); // (before the views: they share it)

	// Our surfaces, in drawing order: each one goes on top of the ones made before it.
	createView();
	createCrosshairAndBanner();
	createLiftArrows();
	createCompass();
	createScannerPanel();
	createWalkModeLines();
	createTopBar();
	createInfoPanel();
	makeMapCamera(Options::fpsMapCamera > 0 ? Options::fpsMapCamera : lastMapCameraSize); // (under the cards)
	createSoldierCard();
	createSoldierCam(); // (last: drawn over everything)

	// The mode's own sounds, and the canvas the overlays are drawn on before being magnified.
	_sounds = new FirstPersonSounds();
	_ambience = new FirstPersonAmbience(); // (its loops are made when the 3D view first shows the battle)
	_scanner = new FirstPersonScanner(_game, _sounds);
	makeCanvas();

	// The weapon in hand (placed every frame, bottom right). The sprites hold the game's
	// 32x48 hand sprites; they aren't added to the state (the destructor deletes them).
	_heldSprite = new Surface(32, 48, 0, 0);
	_heldSprite->setPalette(_state->getPalette());
	_offHandSprite = new Surface(32, 48, 0, 0); // (dual wield: the other hand's)
	_offHandSprite->setPalette(_state->getPalette());
	makeWeaponInHand();
	makeScopeCross();

	// Which corner each surface keeps to on a resize, then the top bar's buttons laid out.
	setResizeAnchors();

	layoutTopBar();

	// A screen remade for the same battle, or right after another one (a game loaded from
	// inside a battle, within 3 seconds), takes over the player's choices. The fuses burning
	// carry over only within the same battle (a loaded game has its grenades as it saved them).
	if (lastBattle == _save || (lastThinkTicks && SDL_GetTicks() - lastThinkTicks < 3000))
	{
		_wants3D = lastWants3D;
		_classicHud = lastClassicHud;
		_hudHidden = lastHudHidden;
		_hudClean = lastHudClean;
		_flashlightOn = lastFlashlightOn;
		_sprintOn = lastSprintOn;
		_enPassantOn = lastEnPassantOn;
		_pausedCheats = lastPausedCheats;
		if (lastBattle == _save) _doomFuses = lastDoomFuses;
		_mouseLook = !_classicHud;
	}
	rememberChoices();
	// VR's calls (see FirstPersonVr.h) come to this screen's mode from now on.
	_vrCurrent = this;
}

/**
 * Makes the 3D view: full screen, inserted right after the isometric map it replaces (so the
 * classic HUD still draws over it). It renders at the canvas size and is magnified with the
 * overlays, so a big screen costs what 640x400 does. It stays hidden until the player
 * switches to 3D (see setActive).
 */
void FirstPersonMode::createView()
{
	int viewWidth, viewHeight;
	wantedViewSize(viewWidth, viewHeight);
	const Mod *mod = _game->getMod();

	// The state adds a surface at the end (on top of everything): move it to right after the
	// isometric map instead.
	_view = new FirstPersonView(_game, viewWidth, viewHeight, 0, 0);
	_view->useVoxelUnits(_voxelUnits.get());
	_view->setUiScale(std::max(1, (int)std::lround(viewWidth / (double)canvasWidth())));
	_state->add(_view);
	auto &surfaces = _state->_surfaces;
	surfaces.pop_back();
	surfaces.insert(std::find(surfaces.begin(), surfaces.end(), _state->_map) + 1, _view);

	// What it shows: this battle, with the isometric map's shots and explosions, as far as the
	// setting "View distance" reaches (the mod's reach at its first value, see viewDistance).
	_view->setSavedBattleGame(_save);
	_view->setMap(_state->_map);
	_view->setMaxDistance(FirstPerson::viewDistance(mod));
	_view->setVisible(false);
	_ownSurfaces.insert(_view);
}

/**
 * Makes the crosshair in the middle of the view, the fire mode and shot texts beside it, and
 * the banner across the top of the view (with a glow around it and a line under it). All of
 * them stay hidden until there is something to show.
 */
void FirstPersonMode::createCrosshairAndBanner()
{
	const int width = canvasWidth(); // our surfaces are laid out on the canvas (see makeCanvas)
	const int height = canvasHeight();

	// The crosshair, centred on the view (drawn in drawCrosshair).
	_crosshair = new Surface(CrosshairSize, CrosshairSize, width / 2 - CrosshairSize / 2, height / 2 - CrosshairSize / 2);
	_state->add(_crosshair);
	// Under it the accuracy line (drawn in drawAccuracyLine): room for its widest reach to either
	// side, the view's edges, and its shadow, its notches two rows above the line.
	_accuracyLineHalf = width / 2;
	_accuracyLine = new Surface(_accuracyLineHalf * 2 + 3, 4, width / 2 - _accuracyLineHalf - 1, height / 2 + AccuracyLineDrop - 2);
	_state->add(_accuracyLine);

	// The banner's glow: four copies of it, one pixel off to each side, behind it (coloured
	// in updateBanner).
	const int glowOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
	for (const auto &offset : glowOffsets)
	{
		Text *glowText = new Text(BannerW, 17, width / 2 - BannerW / 2 + offset[0], 24 + offset[1]);
		_state->add(glowText);
		glowText->setBig();
		glowText->setAlign(ALIGN_CENTER);
		glowText->setVisible(false);
		_ownSurfaces.insert(glowText);
		_bannerGlow.push_back(glowText);
	}

	// The banner itself, in big letters (its shadow too). It must be the first text made with
	// addText: updateBanner colours _textShadows.front() as part of the glow.
	_txtBanner = addText(BannerW, 17, width / 2 - BannerW / 2, 24, "banner", "firstPerson");
	_txtBanner->setBig();
	_textShadows.back()->setBig();

	// The line under the banner: whose camera it was, when a soldier goes down.
	_txtFallName = addText(BannerW, 9, width / 2 - BannerW / 2, 43, "banner", "firstPerson");
	_txtFallName->setAlign(ALIGN_CENTER);
	_txtFallName->setVisible(false);

	// The fire mode and the hit chance to the right of the crosshair, so they don't cover what
	// you aim at.
	_txtMode = addText(160, 9, width / 2 + 14, height / 2 - 9, "crosshairText", "firstPerson"); // (room for a mod's long mode names)
	_txtShot = addText(120, 9, width / 2 + 14, height / 2 + 1, "crosshairText", "firstPerson");
	// Under them, the action's other costs (setting "Expanded crosshair info"): energy, health and
	// the rest, a cost the soldier can't pay in the second colour (red).
	_txtCosts = addText(160, 9, width / 2 + 14, height / 2 + 11, "crosshairText", "firstPerson");
	// Under the hit chance, the shot's damage range (setting "Shot damage"), the costs a line lower.
	_txtDamage = addText(120, 9, width / 2 + 14, height / 2 + 11, "crosshairText", "firstPerson");
	// Left of the crosshair, the range to what it points at (setting "Rangefinder", or its key held).
	_txtRange = addText(90, 9, width / 2 - 104, height / 2 + 1, "crosshairText", "firstPerson");
	_txtRange->setAlign(ALIGN_RIGHT);
	// Left of the crosshair, a line higher: the shot shown won't land under the crosshair (see updateTrajectory, _laserAway).
	_txtAimOff = addText(90, 9, width / 2 - 104, height / 2 - 9, "crosshairText", "firstPerson");
	_txtAimOff->setAlign(ALIGN_RIGHT);
	_txtAimOff->setVisible(false);
	// The analogue rangefinder (drawn in drawRangeScale): its line a fifth of the view wide, its curve at most an eighth
	// of the view high not zoomed in and up to ZoomBoostFactor times that zoomed in, with room for the line's shadow; five
	// numbers over the curve and the height it is drawn for at the line's end.
	_rangeScaleWidth = width / 5;
	_rangeScaleHeight = height / 8;
	_rangeScale = new Surface(_rangeScaleWidth + 3, (int)(_rangeScaleHeight * ZoomBoostFactor) + 8, 0, 0);
	_state->add(_rangeScale);
	_rangeScale->setVisible(false);
	_ownSurfaces.insert(_rangeScale);
	_rangeScaleDrawnFor = ~(Uint64)0;
	_rangeMarks.clear();
	for (int mark = 0; mark < 6; ++mark)
	{
		_rangeMarks.push_back(addText(mark < 5 ? 24 : 40, 9, 0, 0, "crosshairText", "firstPerson"));
		_rangeMarks.back()->setAlign(mark < 5 ? ALIGN_CENTER : ALIGN_RIGHT);
		_rangeMarks.back()->setVisible(false);
	}
	_txtBanner->setAlign(ALIGN_CENTER);
	// The name of the unit pointed at, under the crosshair (it follows the free cursor).
	_txtTarget = addText(160, 9, width / 2 - 80, height / 2 + 10, "crosshairText", "firstPerson");
	_txtTarget->setAlign(ALIGN_CENTER);
	// Zoomed in: SCOPING and the zoom, at the right just under the scope cross's line (clear of
	// the lift arrows' texts above it), left of the visible enemy buttons (see updateScope).
	_txtScoping = addText(90, 9, width - 94, height / 2 + 5, "crosshairText", "firstPerson");
	_txtScoping->setAlign(ALIGN_RIGHT);

	// All hidden until there's something to show.
	for (Text *text : { _txtMode, _txtShot, _txtCosts, _txtDamage, _txtRange, _txtAimOff, _txtBanner, _txtTarget, _txtScoping })
	{
		text->setVisible(false);
	}
	_crosshair->setVisible(false);
	_ownSurfaces.insert(_crosshair);
	_accuracyLine->setVisible(false);
	_ownSurfaces.insert(_accuracyLine);
}

/**
 * Makes the grav lift arrows, one up and one down, with LIFT PLATFORM and a line naming the
 * keys between them, right of the middle of the view (drawn and shown in updateLift).
 */
void FirstPersonMode::createLiftArrows()
{
	const int width = canvasWidth(); // our surfaces are laid out on the canvas (see makeCanvas)
	const int height = canvasHeight();

	// The two arrows, centred on a line 82% of the way across.
	const int liftX = width * 82 / 100;
	_liftUp = new Surface(LiftArrowW, LiftArrowH, liftX - LiftArrowW / 2, height / 2 - 38);
	_liftDown = new Surface(LiftArrowW, LiftArrowH, liftX - LiftArrowW / 2, height / 2 + 24);
	for (Surface *arrow : { _liftUp, _liftDown })
	{
		_state->add(arrow);
		arrow->setVisible(false);
		_ownSurfaces.insert(arrow);
	}

	// LIFT PLATFORM and the keys line between them, in lilac like the arrows.
	_txtLift = addText(100, 9, liftX - 50, height / 2 - 18, "weapon", "firstPerson");
	_txtLiftKeys = addText(100, 9, liftX - 50, height / 2 - 8, "weapon", "firstPerson");
	_txtLift->setText(_state->tr("STR_FPS_LIFT"));
	const Uint8 lilac = textColor(_state->getPalette(), 200, 120, 255); // a group's start: the glyph shades run down it
	for (Text *text : { _txtLift, _txtLiftKeys })
	{
		text->setAlign(ALIGN_CENTER);
		text->setColor(lilac);
		text->setVisible(false);
	}
}

/**
 * Makes the motion scanner's round screen (see FirstPersonScanner): at the bottom middle of the
 * view, about a third of its height across (ScannerPanelHeightShare), as if held up in front of
 * the soldier, where the weapon in hand makes way for it; never over the info panel (on the
 * original 320x200 the middle would be: there it sits just right of it). Hidden until the
 * scanner is used (see updateScanner).
 */
void FirstPersonMode::createScannerPanel()
{
	const int width = canvasWidth(), height = canvasHeight(); // our surfaces are laid out on the canvas (see makeCanvas)
	const int size = std::max(48, (int)(height * ScannerPanelHeightShare) / 2 * 2);
	_scannerPanel = new Surface(size, size, std::max(PanelW + 6, width / 2 - size / 2), height - size - 4);
	_state->add(_scannerPanel);
	_scannerPanel->setVisible(false);
	_ownSurfaces.insert(_scannerPanel);
	_scannerRimColor = textColor(_state->getPalette(), 50, 50, 56);
}

/**
 * Makes the compass (setting "Compass"): a round disc at the top right, under the top bar, with
 * the heading arrow on top of it, the four letters of the compass going round it, and a line under
 * it that warns when looking further will turn the soldier at a cost (see updateCompass). All
 * hidden until the 3D view shows.
 */
void FirstPersonMode::createCompass()
{
	// (Where they all go: see placeCompass, below.)
	_compass = new Surface(CompassSize, CompassArrowH + CompassSize, 0, 0);
	_state->add(_compass);
	_compass->setVisible(false);
	_ownSurfaces.insert(_compass);
	// Beside the dial's left, level with its middle, the climb arrows (see updateClimbArrows).
	_climbArrows = new Surface(ClimbArrowW, ClimbArrowsH, 0, 0);
	_state->add(_climbArrows);
	_climbArrows->setVisible(false);
	_ownSurfaces.insert(_climbArrows);

	// The letters, small, north in red (as on a compass rose), the others in the buttons' grey.
	const char *const letters[4] = { "N", "E", "S", "W" };
	for (int letter = 0; letter < 4; ++letter)
	{
		_compassLetters[letter] = addText(7, 8, 0, 0, "button", "firstPerson");
		_compassLetters[letter]->setSmall();
		_compassLetters[letter]->setAlign(ALIGN_CENTER);
		_compassLetters[letter]->setText(letters[letter]);
		_compassLetters[letter]->setColor(letter == 0 ? textColor(_state->getPalette(), 230, 60, 50) : textColor(_state->getPalette(), 200, 200, 210));
		_compassLetters[letter]->setVisible(false);
	}

	// The warning line under the dial, ending at its right edge (the dial sits against the
	// screen's right edge, and a line centred under it would run off it).
	_txtCompassTurn = addText(CompassSize + 40, 9, 0, 0, "weapon", "firstPerson");
	_txtCompassTurn->setAlign(ALIGN_RIGHT);
	_txtCompassTurn->setVisible(false);

	// What a step each way costs, round the dial, and a level up or down, left of the climb arrows
	// (setting "Compass: step costs", see updateCompassCosts).
	for (Text *&cost : _compassCosts)
	{
		cost = addText(CompassCostW, CompassCostH, 0, 0, "button", "firstPerson");
		cost->setAlign(ALIGN_CENTER);
	}
	for (Text *&cost : _climbCosts)
	{
		cost = addText(CompassCostW, CompassCostH, 0, 0, "button", "firstPerson");
		cost->setAlign(ALIGN_RIGHT);
	}
	for (Text *cost : std::initializer_list<Text*>{ _compassCosts[0], _compassCosts[1], _compassCosts[2], _compassCosts[3],
		_compassCosts[4], _compassCosts[5], _compassCosts[6], _compassCosts[7], _climbCosts[0], _climbCosts[1] })
	{
		cost->setSmall();
		cost->setVisible(false);
	}
	std::fill(std::begin(_compassCostsShown), std::end(_compassCostsShown), -1);
	placeCompass();
}

/**
 * Puts the compass where it belongs, at the top right under the top bar, with the climb arrows
 * left of the dial and the warning line under it. With the steps' costs round the dial (setting
 * "Compass: step costs") it all makes room for them: the dial moves in from the screen's edge and
 * down, the climb arrows further left and the line further down. Called again when the setting
 * changes (see updateCompass); the letters and the costs follow the dial on their own.
 */
void FirstPersonMode::placeCompass()
{
	_compassCostsPlaced = Options::fpsCompassCosts;
	const int roomX = _compassCostsPlaced ? CompassCostRoomX : 0, roomY = _compassCostsPlaced ? CompassCostRoomY : 0;
	// (The numbers above the dial clear the heading arrow, which the dial's surface already has room for.)
	const int compassX = canvasWidth() - CompassSize - 4 - roomX, compassY = BarY + BarH + CompassGap + std::max(0, roomY - CompassArrowH);
	_compass->setX(compassX);
	_compass->setY(compassY);
	const int arrowsX = compassX - roomX - ClimbArrowW - ClimbArrowsGap, arrowsY = compassY + CompassArrowH + CompassSize / 2 - ClimbArrowsH / 2;
	_climbArrows->setX(arrowsX);
	_climbArrows->setY(arrowsY);
	for (int way = 0; way < 2; ++way)
	{
		_climbCosts[way]->setX(arrowsX - CompassCostW - 1);
		_climbCosts[way]->setY(way == 0 ? arrowsY + 1 : arrowsY + ClimbArrowsH - CompassCostH);
	}
	_txtCompassTurn->setX(compassX + roomX - 40);
	_txtCompassTurn->setY(compassY + CompassArrowH + CompassSize + 1 + roomY);
	_compassDrawn[0] = -1000; // (the letters are put round the dial when it's next drawn)
}

/**
 * Makes the lines for the ways of walking switched on with their keys, "SPRINT" in green and
 * "EN PASSANT" in blue, ending at the screen's right edge as the compass's line does; where
 * they go depends on whether the compass shows (see updateWalkModeLines). Hidden for now.
 */
void FirstPersonMode::createWalkModeLines()
{
	const int width = canvasWidth(); // our surfaces are laid out on the canvas (see makeCanvas)
	const int lineWidth = CompassSize + 40, lineX = width - 4 - lineWidth;
	_txtSprint = addText(lineWidth, 9, lineX, 0, "weapon", "firstPerson");
	_txtSprint->setText(_state->tr("STR_FPS_SPRINT"));
	_txtSprint->setColor(textColor(_state->getPalette(), 120, 230, 110));
	_txtEnPassant = addText(lineWidth, 9, lineX, 0, "weapon", "firstPerson");
	_txtEnPassant->setText(_state->tr("STR_FPS_EN_PASSANT"));
	_txtEnPassant->setColor(textColor(_state->getPalette(), 110, 200, 250));
	for (Text *line : { _txtSprint, _txtEnPassant })
	{
		line->setAlign(ALIGN_RIGHT);
		line->setVisible(false);
	}
}

/**
 * Makes the top bar: small buttons in the style of the original UI (colours from the mod's
 * "firstPerson" interface): 3D, HUD, INV, previous / next soldier, the suit's buttons and END.
 * Where each one finally goes is worked out by layoutTopBar. The squad buttons are made as
 * they're needed (see refreshSquad).
 */
void FirstPersonMode::createTopBar()
{
	const int width = canvasWidth(); // our surfaces are laid out on the canvas (see makeCanvas)

	// The fixed buttons: the tools on the left, END on the right, previous / next soldier
	// around the squad buttons. HUD is a toggle (a button group of its own).
	_btnView = makeButton(ViewButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_3D"));
	_btnHud = makeButton(HudButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_HUD"));
	_btnInventory = makeButton(InventoryButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_INVENTORY"));
	_btnEndTurn = makeButton(width - 1 - EndTurnW, EndTurnW, _state->tr("STR_FPS_BUTTON_END_TURN"));
	_btnHud->setGroup(&_hudButtonGroup);
	// (3DIso research: our 3D art or the original art, on the map while 3DIso shows; pressed: the original)
	_btnIsoArt = makeButton(InventoryButtonX, ToolButtonW, "A/R"); // (art or reset, see toggleIsoArt)
	_btnIsoArt->setGroup(&_isoArtButtonGroup);
	_btnIsoArt->setVisible(false);
	// (3DIso research: the free camera, after ART; pressed while it's on; and its marks under the bar's left, see updateIsoFreeMarks)
	_btnIsoFree = makeButton(InventoryButtonX + ToolButtonW + 1, ToolButtonW, "FREE");
	_btnIsoFree->setGroup(&_isoFreeButtonGroup);
	_btnIsoFree->setVisible(false);
	_isoFreeMarks = new Surface(IsoFreeMarksWidth, IsoFreeMarkSize, 1, BarY + BarH + 2);
	_state->add(_isoFreeMarks);
	_isoFreeMarks->setVisible(false);
	_ownSurfaces.insert(_isoFreeMarks);
	_btnPrev = makeButton(SquadX, PrevNextW, "<");
	_btnNext = makeButton(SquadX, PrevNextW, ">");

	// The suit's buttons, shown for what it can do (laid out right to left from END): fly (swim
	// under the sea), personal light, night vision, a special weapon and a tank's turret switch. Each label comes in
	// full and short, for a narrow bar; the buttons start as wide as the full label.
	const bool underwater = _save->getDepth() > 0;
	const char *suitLabels[5][2] = {
		{ underwater ? "STR_FPS_BUTTON_SWIM" : "STR_FPS_BUTTON_FLY", underwater ? "STR_FPS_BUTTON_SWIM_SHORT" : "STR_FPS_BUTTON_FLY_SHORT" },
		{ "STR_FPS_BUTTON_LIGHT", "STR_FPS_BUTTON_LIGHT_SHORT" }, { "STR_FPS_BUTTON_NIGHT", "STR_FPS_BUTTON_NIGHT_SHORT" },
		{ "STR_FPS_BUTTON_SPECIAL", "STR_FPS_BUTTON_SPECIAL_SHORT" }, { "STR_FPS_BUTTON_TURRET", "STR_FPS_BUTTON_TURRET_SHORT" } };
	TextButton **suitButtons[5] = { &_btnFly, &_btnLight, &_btnNight, &_btnSpecial, &_btnTurret };
	for (int buttonIndex = 0; buttonIndex < 5; ++buttonIndex)
	{
		_suitLabelsLong[buttonIndex] = _state->tr(suitLabels[buttonIndex][0]);
		_suitLabelsShort[buttonIndex] = _state->tr(suitLabels[buttonIndex][1]);
		*suitButtons[buttonIndex] = makeButton(0, labelWidth(_suitLabelsLong[buttonIndex]), _suitLabelsLong[buttonIndex]);
		(*suitButtons[buttonIndex])->setVisible(false);
	}

	// Flight, light, night vision and the turret are toggles: each a button group of its own, so
	// the button stays pressed while it's on.
	_btnFly->setGroup(&_flyButtonGroup);
	_btnLight->setGroup(&_lightButtonGroup);
	_btnNight->setGroup(&_nightButtonGroup);
	_btnTurret->setGroup(&_turretButtonGroup);
	// One squad button per soldier, made as they're needed (see refreshSquad).
}

/**
 * Makes the info panel, bottom left: name and status, a line for the weapon in each hand (its
 * name on the left, its rounds and spare clips on the right), the four stats (time units,
 * energy, health, morale) as numbers and bars coloured like the original HUD, and the armor's
 * name with DOOM (the debug mode's tag). It has no background, so the world shows through (the
 * texts have dark shadows). Filled in by updateInfoPanel.
 */
void FirstPersonMode::createInfoPanel()
{
	const int height = canvasHeight(); // our surfaces are laid out on the canvas (see makeCanvas)

	// The panel's top left corner; everything in it is placed from there (canvas pixels).
	const int panelX = 1, panelY = height - PanelH - 1;

	// First line: the name, and the status on the right.
	_panelNameX = panelX + 3; // (the rank, when there is one, goes here and the name after it)
	_txtName = addText(PanelW - 66 - RankIconW - RankIconGap, 9, _panelNameX, panelY + 2, "textName", "battlescape");
	_txtStatus = addText(60, 9, panelX + PanelW - 63, panelY + 2, "weapon", "firstPerson");

	// Then a line for each hand: the weapon in the hand that fires, then the other hand's item.
	// Each line is two texts: the hand and the name, and the ammo, which fillHandLine places
	// right after the name (and shortens a name that would leave no room for it).
	_txtWeapon = addText(PanelLineW, 9, panelX + 3, panelY + 11, "weapon", "firstPerson");
	_txtWeaponAmmo = addText(PanelLineW, 9, panelX + 3, panelY + 11, "weapon", "firstPerson");
	_txtOtherWeapon = addText(PanelLineW, 9, panelX + 3, panelY + 20, "weapon", "firstPerson");
	_txtOtherAmmo = addText(PanelLineW, 9, panelX + 3, panelY + 20, "weapon", "firstPerson");

	// The four stats in two columns, each a number with its bar beside it, in the original
	// HUD's colours.
	_numTimeUnits = addNumber(panelX + 3, panelY + 30, "numTUs");
	_barTimeUnits = new Bar(52, 3, panelX + 19, panelY + 31);
	_numEnergy = addNumber(panelX + 76, panelY + 30, "numEnergy");
	_barEnergy = new Bar(52, 3, panelX + 92, panelY + 31);
	_numHealth = addNumber(panelX + 3, panelY + 36, "numHealth");
	_barHealth = new Bar(52, 3, panelX + 19, panelY + 37);
	_numMorale = addNumber(panelX + 76, panelY + 36, "numMorale");
	_barMorale = new Bar(52, 3, panelX + 92, panelY + 37);
	_state->add(_barTimeUnits, "barTUs", "battlescape");
	_state->add(_barEnergy, "barEnergy", "battlescape");
	_state->add(_barHealth, "barHealth", "battlescape");
	_state->add(_barMorale, "barMorale", "battlescape");
	// A mod's mana (Piratez's freshness, X-Com Files' sanity: a player, GitHub issue #77): a thin
	// bar under the stats, across both columns, in the mod's mana colour, as the map shows it (no
	// number, as there); shown only when the map shows it (see updateInfoPanel).
	_barMana = new Bar(PanelManaW, PanelManaH, panelX + PanelManaX, panelY + PanelManaY);
	_state->add(_barMana, "barMana", "battlescape");

	// Last line: the armor, and DOOM on the right.
	_txtArmor = addText(PanelLineW, 9, panelX + 3, panelY + 44, "weapon", "firstPerson");
	_txtDoom = addText(60, 9, panelX + PanelW - 63, panelY + 44, "doom", "firstPerson");

	// The soldier's rank, a small badge before the name (see drawRankIcon).
	_panelRank = new Surface(RankIconW, RankIconH, 0, 0);
	_state->add(_panelRank);

	// The panel's parts, all ours and all hidden until the 3D view shows.
	_panelParts = { _txtName, _panelRank, _txtStatus, _txtWeapon, _txtWeaponAmmo, _txtOtherWeapon, _txtOtherAmmo, _numTimeUnits, _barTimeUnits,
		_numEnergy, _barEnergy, _numHealth, _barHealth, _numMorale, _barMorale, _barMana, _txtArmor, _txtDoom };
	for (auto *part : _panelParts)
	{
		_ownSurfaces.insert(part);
		part->setVisible(false);
	}

	// Right-aligned texts, the weapons' own colour (the ammo turns red when a gun is empty, the
	// other hand's line grey: see fillHandLine), the fixed DOOM tag, and bars at one pixel per
	// point.
	_txtDoom->setAlign(ALIGN_RIGHT);
	_txtStatus->setAlign(ALIGN_RIGHT);
	_weaponTextColor = _txtWeapon->getColor();
	_txtDoom->setText(_state->tr("STR_FPS_DOOM"));
	for (auto *bar : { _barTimeUnits, _barEnergy, _barHealth, _barMorale, _barMana })
	{
		bar->setScale(1.0);
	}
	// The stats' own colours, for when Doom IDDQD has turned them silver and stops.
	const NumberText *numbers[4] = { _numTimeUnits, _numEnergy, _numHealth, _numMorale };
	const Bar *bars[4] = { _barTimeUnits, _barEnergy, _barHealth, _barMorale };
	static const char *const barIds[4] = { "barTUs", "barEnergy", "barHealth", "barMorale" };
	const RuleInterface *battlescapeRules = _game->getMod()->getInterface("battlescape", false);
	for (int stat = 0; stat < 4; ++stat)
	{
		_statColors[stat] = numbers[stat]->getColor();
		_statColors[4 + stat] = bars[stat]->getColor();
		const Element *barElement = battlescapeRules ? battlescapeRules->getElementOptional(barIds[stat]) : nullptr;
		_statBorders[stat] = barElement && barElement->border != INT_MAX ? (Uint8)barElement->border : 0;
	}
}

/**
 * Makes the soldier card, shown under a hovered squad button: name, weapon, status, the four
 * stats, and a small live view from the soldier's helmet camera (with "signal lost" over it
 * when there's no picture). Each part remembers where it goes on the card; the card is placed
 * and filled each frame in updateSoldierCard.
 */
void FirstPersonMode::createSoldierCard()
{
	const Mod *mod = _game->getMod();

	// The card's background.
	_card = new Surface(CardW, 40 + PreviewH, 0, CardY);
	_state->add(_card);
	_cardParts.push_back({ _card, 0, 0 });

	// The helmet camera: a small second 3D view, sharing the big view's voxel models, reaching
	// at most 30 tiles and without particles, to stay cheap.
	_preview = new FirstPersonView(_game, PreviewW, PreviewH, 0, CardY + 36);
	_state->add(_preview);
	_preview->shareCache(_view); // (first: then the battle doesn't build models of its own)
	_preview->useVoxelUnits(_voxelUnits.get());
	_preview->setSavedBattleGame(_save);
	_preview->setMap(_state->_map);
	_preview->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	_preview->setParticles(false);
	_preview->setSignalGlitches(mod->getFirstPerson().getSignalLevel(_save->getMissionType())); // a camera's feed: a bad signal now and then (a lot on the hardest missions)
	_cardParts.push_back({ _preview, 4, 36 });

	// The texts: name, weapon, status (top right), and "signal lost" across the middle of
	// the camera picture.
	_cardName = new Text(CardW - 8, 9, 0, 0);
	_state->add(_cardName, "textName", "battlescape");
	_cardParts.push_back({ _cardName, 4, 3 });
	_cardRank = new Surface(RankIconW, RankIconH, 0, 0); // (before the name: placed in updateSoldierCard)
	_state->add(_cardRank);
	_cardParts.push_back({ _cardRank, 4, 3 - RankIconRise });
	_cardIcon = new Surface(CraftIconW, CraftIconH, 0, 0); // (before the rank and name, and the weapon line: see updateSoldierCard)
	_state->add(_cardIcon);
	_cardParts.push_back({ _cardIcon, CraftIconX, CraftIconY });
	_cardWeapon = new Text(CardW - 8, 9, 0, 0);
	_state->add(_cardWeapon, "weapon", "firstPerson");
	_cardParts.push_back({ _cardWeapon, 4, 12 });
	_cardStatus = new Text(80, 9, 0, 0);
	_state->add(_cardStatus, "doom", "firstPerson");
	_cardStatus->setAlign(ALIGN_RIGHT);
	_cardParts.push_back({ _cardStatus, CardW - 84, 3 });
	_cardSignal = new Text(PreviewW, 9, 0, 0);
	_state->add(_cardSignal, "doom", "firstPerson");
	_cardSignal->setAlign(ALIGN_CENTER);
	_cardSignal->setText(_state->tr("STR_FPS_SIGNAL_LOST"));
	_cardParts.push_back({ _cardSignal, 4, 36 + PreviewH / 2 - 4 });

	// The four stats (time units, energy, health, morale) in two columns of two, each a number
	// with its bar beside it, in the original HUD's colours.
	const char *numberIds[4] = { "numTUs", "numEnergy", "numHealth", "numMorale" };
	const char *barIds[4] = { "barTUs", "barEnergy", "barHealth", "barMorale" };
	for (int stat = 0; stat < 4; ++stat)
	{
		const int offsetX = 4 + (stat % 2) * 60, offsetY = 22 + (stat / 2) * 6; // from the card's top left
		_cardNumbers[stat] = new NumberText(15, 5, 0, 0);
		_state->add(_cardNumbers[stat], numberIds[stat], "battlescape");
		_cardParts.push_back({ _cardNumbers[stat], offsetX, offsetY });
		_cardBars[stat] = new Bar(40, 3, 0, 0);
		_state->add(_cardBars[stat], barIds[stat], "battlescape");
		_cardBars[stat]->setScale(1.0);
		_cardParts.push_back({ _cardBars[stat], offsetX + 16, offsetY + 1 });
	}

	// The texts in high contrast, to read over the card; every part ours and hidden for now.
	for (Text *text : { _cardName, _cardWeapon, _cardStatus, _cardSignal })
	{
		text->setHighContrast(true);
	}
	for (const auto &part : _cardParts)
	{
		part.surface->setVisible(false);
		_ownSurfaces.insert(part.surface);
	}
}

/**
 * A live feed's red dot (the soldier cam's, the map camera's): a filled circle 5 pixels across,
 * added to the screen, hidden; the camera blinks it.
 * @return The dot.
 */
Surface *FirstPersonMode::makeLiveDot()
{
	Surface *dot = new Surface(5, 5, 0, 0);
	_state->add(dot);
	dot->setPalette(_state->getPalette());
	const Uint8 red = nearestColor(_state->getPalette(), 230, 30, 30);
	dot->lock();
	for (int dotY = 0; dotY < 5; ++dotY)
	{
		for (int dotX = 0; dotX < 5; ++dotX)
		{
			if ((dotX - 2) * (dotX - 2) + (dotY - 2) * (dotY - 2) <= 5) dot->setPixel(dotX, dotY, red);
		}
	}
	dot->unlock();
	dot->setVisible(false);
	_ownSurfaces.insert(dot);
	return dot;
}

/**
 * Makes the soldier cam, hidden: a soldier's card's helmet camera enlarged, with a right click
 * on their squad button (see updateSoldierCam). A frame with a title bar ("SOLDIER CAM: name",
 * a blinking red dot, as a live feed has) round a second small 3D view, with a little TV
 * static over it, so it's plain that it's a camera, not the view the player plays in. The
 * picture and the frame are made at the canvas's size (see makeSoldierCamView).
 */
void FirstPersonMode::createSoldierCam()
{
	makeSoldierCamView();

	_soldierCamDot = makeLiveDot();

	// The title.
	_txtSoldierCam = new Text(220, 9, 0, 0);
	_state->add(_txtSoldierCam, "textName", "battlescape");
	_txtSoldierCam->setHighContrast(true);
	_soldierCamRank = new Surface(RankIconW, RankIconH, 0, 0); // the soldier's rank, before the title
	_state->add(_soldierCamRank);

	for (Surface *part : { (Surface*)_soldierCamDot, (Surface*)_txtSoldierCam, _soldierCamRank })
	{
		part->setVisible(false);
		_ownSurfaces.insert(part);
	}
}

/**
 * Makes (or makes again, after a resize) the soldier cam's picture and frame at the canvas's
 * size: about 60% of its width (at least twice the card's camera, and inside the canvas), in the
 * card camera's proportions. The picture is a second small 3D view like the card's: sharing the
 * big view's voxel models, reaching at most 30 tiles, without particles; it takes the old one's
 * place among the surfaces.
 */
void FirstPersonMode::makeSoldierCamView()
{
	_soldierCamWidth = std::min(canvasWidth() - 24, std::max(2 * PreviewW, canvasWidth() * 3 / 5)) & ~1;
	_soldierCamHeight = _soldierCamWidth * PreviewH / PreviewW;
	const Mod *mod = _game->getMod();

	// The frame: the picture inside a 4 pixel border, under a 16 pixel title bar, in the card's
	// colours.
	Surface *frame = new Surface(_soldierCamWidth + 8, _soldierCamHeight + 20, 0, 0);
	frame->setPalette(_state->getPalette());
	Uint8 background, border;
	panelColors(background, border);
	frame->drawRect(0, 0, frame->getWidth(), frame->getHeight(), border);
	frame->drawRect(1, 1, frame->getWidth() - 2, frame->getHeight() - 2, background);
	frame->drawRect(3, 15, _soldierCamWidth + 2, _soldierCamHeight + 2, border);

	// The picture.
	FirstPersonView *camera = new FirstPersonView(_game, _soldierCamWidth, _soldierCamHeight, 0, 0);
	camera->setPalette(_state->getPalette());
	camera->shareCache(_view); // (first: then the battle doesn't build models of its own)
	camera->useVoxelUnits(_voxelUnits.get());
	camera->setSavedBattleGame(_save);
	camera->setMap(_state->_map);
	camera->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	camera->setParticles(false);
	camera->setStaticSpells(true); // a camera's picture: a spell of TV static now and then
	camera->setSignalGlitches(mod->getFirstPerson().getSignalLevel(_save->getMissionType())); // and a digital feed's bad signal now and then (a lot on the hardest missions)

	// In place of the old ones (or, the first time, added last: drawn over everything).
	putSurfaceInPlace(_soldierCamFrame, frame);
	putSurfaceInPlace(_soldierCam, camera);
	_soldierCamFrame = frame;
	_soldierCam = camera;
}

/**
 * Puts a surface in another's place in the screen's two lists: the order it draws them in, and
 * the surfaces it deletes when it goes. Both: a surface remade in a resize (the 3D view, the
 * soldier cam, the map camera, the scope cross) whose old one stayed in the second list was
 * deleted again when the battle ended (a player's crash abandoning a mission, State::~State;
 * the more resizes, the likelier).
 * @param oldSurface The surface going (the caller deletes it).
 * @param newSurface The one taking its place.
 */
void FirstPersonMode::replaceStateSurface(Surface *oldSurface, Surface *newSurface)
{
	std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), oldSurface, newSurface);
	std::replace(_state->_surfacesOwned.begin(), _state->_surfacesOwned.end(), oldSurface, newSurface);
}

/**
 * Puts a remade surface of ours in the old one's place among the state's surfaces (so it's drawn
 * in the same order), shown or hidden as that was, and deletes the old one; the first time (no
 * old one) it's added last, hidden: drawn over everything made so far.
 * @param oldSurface The surface it replaces, or null.
 * @param newSurface The new surface.
 */
void FirstPersonMode::putSurfaceInPlace(Surface *oldSurface, Surface *newSurface)
{
	const bool wasVisible = oldSurface && oldSurface->getVisible();
	if (oldSurface)
	{
		replaceStateSurface(oldSurface, newSurface);
		_ownSurfaces.erase(oldSurface);
		delete oldSurface;
	}
	else
	{
		_state->add(newSurface);
	}
	newSurface->setVisible(wasVisible);
	_ownSurfaces.insert(newSurface);
}

/**
 * The colours of our panels and cards (the info panel's, the soldier card's, the cameras'
 * frames): the mod's "firstPerson" interface, element "panel", if it sets them, else a black
 * background with a grey border.
 * @param background Receives the background's palette colour.
 * @param border Receives the border's.
 */
void FirstPersonMode::panelColors(Uint8 &background, Uint8 &border) const
{
	background = 15;
	border = 8;
	if (const RuleInterface *interfaceRules = _game->getMod()->getInterface("firstPerson", false))
	{
		if (const Element *panelElement = interfaceRules->getElementOptional("panel"))
		{
			if (panelElement->color != INT_MAX) background = panelElement->color;
			if (panelElement->border != INT_MAX) border = panelElement->border;
		}
	}
}

/**
 * Makes (or makes again, at another size) the map camera's picture and frame (see
 * updateMapCamera). The picture is a small 3D view like the card's camera: sharing the big
 * view's voxel models, reaching at most 30 tiles, without particles, with the helmet cameras'
 * bad signal; the frame has the card's colours, a border round the picture and a title line
 * above it. They take the old ones' places among the surfaces (the first time they're added,
 * then the title over them).
 * @param size The setting "Map camera"'s size: 1 small, 2 medium, 3 large.
 */
void FirstPersonMode::makeMapCamera(int size)
{
	// The picture's size: its share of the canvas's width, in the card camera's proportions,
	// and not too tall for the canvas (even widths).
	_mapCameraSize = std::clamp(size, 1, 3);
	_mapCameraCanvasWidth = canvasWidth();
	int pictureWidth = (int)(canvasWidth() * MapCameraShare[_mapCameraSize]) & ~1;
	int pictureHeight = pictureWidth * PreviewH / PreviewW;
	const int tallest = (int)(canvasHeight() * MapCameraMaxHeight);
	if (pictureHeight > tallest)
	{
		pictureHeight = tallest;
		pictureWidth = (pictureHeight * PreviewW / PreviewH) & ~1;
	}
	const Mod *mod = _game->getMod();

	// The frame: the picture inside a border, under the title line.
	Surface *frame = new Surface(pictureWidth + 2 * MapCameraBorder, pictureHeight + MapCameraTitleH + 2 * MapCameraBorder, 0, 0);
	frame->setPalette(_state->getPalette());
	Uint8 background, border;
	panelColors(background, border);
	frame->drawRect(0, 0, frame->getWidth(), frame->getHeight(), border);
	frame->drawRect(1, 1, frame->getWidth() - 2, frame->getHeight() - 2, background);
	frame->drawRect(MapCameraBorder - 1, MapCameraBorder + MapCameraTitleH - 1, pictureWidth + 2, pictureHeight + 2, border);

	// The picture.
	FirstPersonView *camera = new FirstPersonView(_game, pictureWidth, pictureHeight, 0, 0);
	camera->setPalette(_state->getPalette());
	camera->shareCache(_view); // (first: then the battle doesn't build models of its own)
	camera->useVoxelUnits(_voxelUnits.get());
	camera->setSavedBattleGame(_save);
	camera->setMap(_state->_map);
	camera->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	camera->setParticles(false);
	camera->setStaticSpells(true); // a helmet camera's feed, as the soldier cam's: static now and then, and the bad signal
	camera->setSignalGlitches(mod->getFirstPerson().getSignalLevel(_save->getMissionType()));

	putSurfaceInPlace(_mapCameraFrame, frame);
	putSurfaceInPlace(_mapCamera, camera);
	_mapCameraFrame = frame;
	_mapCamera = camera;

	// The title (made the first time), as wide as the picture less the live feed's red dot before it.
	if (!_txtMapCamera)
	{
		_mapCameraDot = makeLiveDot();
		_txtMapCamera = new Text(pictureWidth, 9, 0, 0);
		_state->add(_txtMapCamera, "textName", "battlescape");
		_txtMapCamera->setHighContrast(true);
		_txtMapCamera->setVisible(false);
		_ownSurfaces.insert(_txtMapCamera);
	}
	_txtMapCamera->setWidth(pictureWidth - 10);
	_txtMapCamera->setText(""); // (written again for the new width, see updateMapCamera)
}

/**
 * Decides which screen corner (or edge middle) each of our surfaces keeps to when the screen
 * is resized (see shiftAnchored). Everything not listed stays top left.
 */
void FirstPersonMode::setResizeAnchors()
{
	// The banner across the top keeps to the top middle.
	for (Text *text : _bannerGlow) _anchors[text] = TOP_CENTER;
	_anchors[_txtBanner] = TOP_CENTER;
	_anchors[_txtFallName] = TOP_CENTER;

	// The crosshair and its texts keep to the middle; the lift arrows to the right edge's middle.
	_anchors[_crosshair] = CENTER;
	_anchors[_accuracyLine] = CENTER;
	for (Surface *part : std::initializer_list<Surface*>{ _liftUp, _liftDown, _txtLift, _txtLiftKeys }) _anchors[part] = RIGHT_CENTER;
	_anchors[_txtMode] = CENTER;
	_anchors[_txtShot] = CENTER;
	_anchors[_txtCosts] = CENTER;
	_anchors[_txtDamage] = CENTER;
	_anchors[_txtRange] = CENTER;
	_anchors[_txtAimOff] = CENTER;
	_anchors[_rangeScale] = CENTER;
	for (Text *mark : _rangeMarks) _anchors[mark] = CENTER;
	_anchors[_txtScoping] = RIGHT_CENTER;

	// END and the suit's buttons keep to the top right, and so does the compass; the info panel
	// to the bottom left.
	_anchors[_btnEndTurn] = TOP_RIGHT;
	_anchors[_compass] = TOP_RIGHT;
	_anchors[_climbArrows] = TOP_RIGHT;
	for (Text *letter : _compassLetters) _anchors[letter] = TOP_RIGHT;
	for (Text *cost : _compassCosts) _anchors[cost] = TOP_RIGHT;
	for (Text *cost : _climbCosts) _anchors[cost] = TOP_RIGHT;
	_anchors[_txtCompassTurn] = TOP_RIGHT;
	_anchors[_txtSprint] = TOP_RIGHT;
	_anchors[_txtEnPassant] = TOP_RIGHT;
	for (Surface *part : _panelParts) _anchors[part] = BOTTOM_LEFT;
	for (Surface *part : { _btnFly, _btnLight, _btnNight, _btnSpecial, _btnTurret }) _anchors[part] = TOP_RIGHT;
}


/**
 * Notes what the player has chosen (3D or not, the classic HUD, the flashlight, sprinting and
 * en passant, what the cheats and akimbo keys switch back on) and the Doom fuses burning, for a
 * remade screen of the same battle. Called the moment a choice is made, and every frame (for the
 * fuses): the old screen is only deleted after the new one is made, so waiting for its
 * destructor would be too late.
 */
void FirstPersonMode::rememberChoices() const
{
	lastBattle = _save;
	lastWants3D = _wants3D;
	lastClassicHud = _classicHud;
	lastHudHidden = _hudHidden;
	lastHudClean = _hudClean;
	lastFlashlightOn = _flashlightOn;
	lastSprintOn = _sprintOn;
	lastEnPassantOn = _enPassantOn;
	lastPausedCheats = _pausedCheats;
	lastDoomFuses = _doomFuses;
}

/**
 * The screen was resized: BattlescapeState moved every surface by half the width change and
 * the whole height change, like its bottom-centred HUD. Ours keep to their own corners instead
 * (shadows follow their texts), and the 3D view is made again at the new size.
 * @param widthChange How much wider the screen got, in pixels (negative: narrower).
 * @param heightChange How much taller the screen got, in pixels (negative: shorter).
 */
void FirstPersonMode::resized(int widthChange, int heightChange)
{
	// A resize to the same size (a window manager can answer every change of video mode with
	// one, as the inventory's own screen size makes): nothing to do (it remade the 3D views,
	// freezing the game for a moment each time).
	if (widthChange == 0 && heightChange == 0)
	{
		return;
	}

	// The battlescape moved everything to the middle and the bottom; undo that for our
	// overlays, which live on the canvas.
	for (Surface *surface : _ownSurfaces)
	{
		if (surface == _view)
		{
			continue;
		}
		surface->setX(surface->getX() - widthChange / 2);
		surface->setY(surface->getY() - heightChange);
	}

	_warningY += heightChange; // the HUD's warning place moved with the HUD
	relayout();
}

/**
 * What the canvas is magnified onto now: the game's screen; or, while 3D shows at the display's
 * own size (setting "3D resolution": Display), the display's picture area, so the HUD has a
 * scale of its own there, whatever the battlescape's (see blit). Over the isometric map the
 * overlays stay on the game's screen with the map.
 * @param width, height Its size, in its own pixels.
 * @return True for the display.
 */
bool FirstPersonMode::canvasTarget(int &width, int &height) const
{
	return canvasTargetWhen(isActive(), width, height);
}

/**
 * What the canvas is magnified onto with 3D showing, or with the map (see canvasTarget; primeView asks what it will
 * be once 3D shows).
 * @param active Whether 3D shows.
 * @param width, height Its size, in its own pixels.
 * @return True for the display.
 */
bool FirstPersonMode::canvasTargetWhen(bool active, int &width, int &height) const
{
	width = Options::baseXResolution;
	height = Options::baseYResolution;
	// (not with the Live command feed: it draws all of the game's screen itself, the 3D view in its window, see blitFeed)
	if (!active || feedWanted() || Options::fpsViewResolution != FirstPerson::ViewResolutionDisplay || !FirstPerson::canPresentAtDisplaySize())
	{
		return false;
	}
	int x, y, pictureWidth, pictureHeight;
	_game->getScreen()->getPictureArea(x, y, pictureWidth, pictureHeight);
	// (A picture area the screen's own size needs no laying in, but for the full-colour picture, which the 8-bit screen can't hold.)
	const bool fullColor = Options::fpsTrueColor && Options::fpsRenderer == 1;
	if (pictureWidth <= 0 || pictureHeight <= 0 || (pictureWidth == width && pictureHeight == height && !fullColor))
	{
		return false;
	}
	width = pictureWidth;
	height = pictureHeight;
	return true;
}

/**
 * Lays the overlays out again if what the canvas is magnified onto, or by how much, is no longer
 * what it was laid out for: the setting "HUD scale" or "3D resolution" changed, the display
 * did, or 3D was switched to or from with the view at the display's size.
 */
void FirstPersonMode::relayoutIfNeeded()
{
	int width, height;
	const bool onDisplay = canvasTarget(width, height);
	if (onDisplay != _canvasOnDisplay || width != _targetWidth || height != _targetHeight || uiScaleFor(width, height) != _uiScale)
	{
		relayout();
	}
	else
	{
		remakeViewIfResized(); // (the setting "3D resolution" between HUD and Battlescape: only the view's size)
	}
}

/**
 * Lays the overlays out for the canvas as it should be now (see canvasTarget and uiScaleFor):
 * works out how much the canvas grew, moves each overlay with its corner, and makes the canvas,
 * the weapon in hand and whatever else has the canvas's measure again; a new 3D view if its
 * size changed.
 */
void FirstPersonMode::relayout()
{
	const int oldWidth = canvasWidth(), oldHeight = canvasHeight();
	_canvasOnDisplay = canvasTarget(_targetWidth, _targetHeight);
	_uiScale = uiScaleFor(_targetWidth, _targetHeight);
	shiftAnchored(canvasWidth() - oldWidth, canvasHeight() - oldHeight);
	makeCanvas();
	makeWeaponInHand();
	makeScopeCross();

	// A new 3D view at the new size (only when it changed: the magnification can take up a
	// change of screen size); the same view keeps its size but not its pixels to the canvas's.
	const FirstPersonView *viewBefore = _view;
	remakeViewIfResized();
	if (_view == viewBefore && (canvasWidth() != oldWidth || canvasHeight() != oldHeight))
	{
		_view->setUiScale(viewScale());
		makeSoldierCamView(); // (its size follows the canvas)
	}

	// The game's warning message: in full 3D it goes above our info panel.
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	layoutTopBar();
}

/**
 * Makes the 3D view again, set up like the old one and put in its place, when its size isn't
 * what it should be now (see wantedViewSize). After the screen or the display changed size, or
 * the setting "3D resolution".
 */
void FirstPersonMode::remakeViewIfResized()
{
	int viewWidth, viewHeight;
	wantedViewSize(viewWidth, viewHeight);
	if (viewWidth == _view->getWidth() && viewHeight == _view->getHeight())
	{
		return;
	}
	FirstPersonView *newView = new FirstPersonView(_game, viewWidth, viewHeight, 0, 0);
	newView->setUiScale(std::max(1, (int)std::lround(viewWidth / (double)canvasWidth())));
	newView->setPalette(_state->getPalette());
	newView->useVoxelUnits(_voxelUnits.get());
	newView->setSavedBattleGame(_save);
	newView->setMap(_state->_map);
	newView->setMaxDistance(FirstPerson::viewDistance(_game->getMod()));
	newView->setVisible(_view->getVisible());
	replaceStateSurface(_view, newView);
	_ownSurfaces.erase(_view);
	_ownSurfaces.insert(newView);
	_preview->shareCache(newView);
	_mapCamera->shareCache(newView);
	if (_isoView) _isoView->shareCache(newView); // (3DIso research: it borrows the 3D view's models too)
	delete _view;
	_view = newView;
	_trackedUnit = nullptr; // re-place the camera
	_viewPrimed = false; // (a new view sends the world to the card anew: see primeView)
	makeSoldierCamView(); // (its size follows the canvas)
}

/**
 * How much bigger the overlays are drawn on a screen of this size: about one step per 360
 * lines of the game's screen, never so big that the canvas is narrower than the original 320.
 * @param width The game's screen width, in pixels.
 * @param height The game's screen height, in pixels.
 * @return The magnification, 1 or more.
 */
int FirstPersonMode::uiScaleFor(int width, int height)
{
	// The biggest magnification the canvas's layout still fits at; the setting "HUD scale" may ask for a smaller one.
	const int biggest = std::max(1, std::min(height / 360, width / 320));
	return Options::fpsHudScale > 0 ? std::min(Options::fpsHudScale, biggest) : biggest;
}

/**
 * Gets the width of the canvas our overlays are laid out on: the width of what it is magnified
 * onto (the game's screen, or the display: see canvasTarget) divided by the magnification.
 */
int FirstPersonMode::canvasWidth() const
{
	return _targetWidth / _uiScale;
}

/**
 * Gets the height of the canvas our overlays are laid out on, likewise.
 */
int FirstPersonMode::canvasHeight() const
{
	return _targetHeight / _uiScale;
}

/**
 * The size the 3D view should have now (setting "3D resolution"):
 *  - HUD: the canvas's, drawn magnified with the overlays (a big screen costs what a small one does);
 *  - Battlescape: the game's own screen's, the canvas's times the UI scale: only the overlays are magnified;
 *  - Display: the display's own (its picture area, inside any black bands), whatever the
 *    battlescape's size: the view is then shown past the game's screen (see blit and
 *    FirstPerson::presentView).
 */
void FirstPersonMode::wantedViewSize(int &width, int &height) const
{
	// (with the Live command feed: its 3D window's picture, see feedViewSize)
	if (feedViewSize(width, height))
	{
		return;
	}
	width = canvasWidth();
	height = canvasHeight();
	if (Options::fpsViewResolution == FirstPerson::ViewResolutionDisplay && FirstPerson::canPresentAtDisplaySize())
	{
		int x, y;
		_game->getScreen()->getPictureArea(x, y, width, height);
	}
	else if (Options::fpsViewResolution != FirstPerson::ViewResolutionHud)
	{
		width *= _uiScale;
		height *= _uiScale;
	}
}

/**
 * A point of the game's screen (where the mouse is reported, and the game's own surfaces are
 * placed) as the canvas's, across: divided by the magnification; with the canvas on the display
 * (see canvasTarget) also by how many display pixels a screen pixel takes.
 */
double FirstPersonMode::toCanvasX(double screenX) const
{
	return _canvasOnDisplay ? screenX * _targetWidth / ((double)Options::baseXResolution * _uiScale) : screenX / _uiScale;
}

/// The same, down.
double FirstPersonMode::toCanvasY(double screenY) const
{
	return _canvasOnDisplay ? screenY * _targetHeight / ((double)Options::baseYResolution * _uiScale) : screenY / _uiScale;
}

/// A length on the canvas in the game's screen's pixels, across (rounded up: room for it).
int FirstPersonMode::toScreenX(int canvasX) const
{
	return _canvasOnDisplay ? (int)std::ceil(canvasX * (double)_uiScale * Options::baseXResolution / _targetWidth) : canvasX * _uiScale;
}

/// The same, down.
int FirstPersonMode::toScreenY(int canvasY) const
{
	return _canvasOnDisplay ? (int)std::ceil(canvasY * (double)_uiScale * Options::baseYResolution / _targetHeight) : canvasY * _uiScale;
}

/**
 * Whether the view is the display's own size and so shown past the game's screen: asked for by
 * the setting, possible, and the view made that size (and not also the game's screen's size:
 * then it is simply drawn into it).
 */
bool FirstPersonMode::viewAtDisplaySize() const
{
	if (Options::fpsViewResolution != FirstPerson::ViewResolutionDisplay || !FirstPerson::canPresentAtDisplaySize())
	{
		return false;
	}
	int x, y, width, height;
	_game->getScreen()->getPictureArea(x, y, width, height);
	return _view->getWidth() == width && _view->getHeight() == height && (width != Options::baseXResolution || height != Options::baseYResolution);
}

/**
 * About how many of the view's pixels a canvas pixel spans, 1 or more: the size of what the view
 * and the mode draw by the pixel on it (its markers and icons, the room under the pointer).
 */
int FirstPersonMode::viewScale() const
{
	return std::max(1, (int)std::lround(_view->getWidth() / (double)canvasWidth()));
}

/**
 * The view's pixel at the middle of a canvas pixel, across. The crosshair, the free cursor's
 * place and the clicks are the canvas's; the view's rays go through its own pixels. With the
 * view the canvas's size they are the same pixels; with a bigger view a canvas pixel covers
 * several of the view's, and the one at its middle is taken, so a shot goes through the same
 * point of the picture at any resolution (to within half a view pixel).
 */
int FirstPersonMode::viewPixelX(int canvasPixel) const
{
	return std::min(_view->getWidth() - 1, (int)((canvasPixel + 0.5) * _view->getWidth() / canvasWidth()));
}

/// The same, down.
int FirstPersonMode::viewPixelY(int canvasPixel) const
{
	return std::min(_view->getHeight() - 1, (int)((canvasPixel + 0.5) * _view->getHeight() / canvasHeight()));
}

/**
 * How far the aim's ray goes from the middle of the view's pixel (see viewPixelX), in view pixels.
 * The crosshair's middle is the middle of a canvas pixel. With the view an odd number of times
 * the canvas's size, a view pixel has its middle there too; with an even number (HUD scale x2,
 * x4 with 3D resolution Display) that point is the corner of four view pixels, and the one
 * taken has its middle half a pixel down and to the right of it: the ray goes back that half.
 */
double FirstPersonMode::aimPixelShift() const
{
	return viewScale() % 2 == 0 ? -0.5 : 0.0;
}

/// The canvas pixel a pixel of the view lies in, across.
int FirstPersonMode::canvasPixelX(int viewPixel) const
{
	SDL_Rect picture;
	if (feedViewPicture(picture)) return (int)std::lround(toCanvasX(picture.x + (double)viewPixel * picture.w / _view->getWidth())); // (the Live command feed's 3D window)
	return (int)((long long)viewPixel * canvasWidth() / _view->getWidth());
}

/// The same, down.
int FirstPersonMode::canvasPixelY(int viewPixel) const
{
	SDL_Rect picture;
	if (feedViewPicture(picture)) return (int)std::lround(toCanvasY(picture.y + (double)viewPixel * picture.h / _view->getHeight())); // (the Live command feed's 3D window)
	return (int)((long long)viewPixel * canvasHeight() / _view->getHeight());
}

/**
 * Makes (again) the canvas the overlays are drawn on before being magnified onto the screen.
 * At scale 1 none is needed: everything is drawn straight onto the screen. With the canvas on
 * the display (see canvasTarget) there is always one, and the display-sized picture it is put
 * together on with the view (see blit).
 */
void FirstPersonMode::makeCanvas()
{
	delete _canvas;
	_canvas = nullptr;
	delete _composed;
	_composed = nullptr;
	if (_canvasOnDisplay)
	{
		_composed = new Surface(_targetWidth, _targetHeight, 0, 0);
	}
	if (_uiScale > 1 || _canvasOnDisplay)
	{
		// The canvas keeps its own copy of the palette (see magnify, which keeps it current).
		_canvas = new Surface(canvasWidth(), canvasHeight(), 0, 0);
		std::copy(_state->getPalette(), _state->getPalette() + 256, _canvasPalette);
		_canvas->setPalette(_canvasPalette);
	}
}

/**
 * Moves the overlays for a canvas that grew, each keeping to its corner (see setResizeAnchors):
 * a surface kept to the right edge moves the whole width change, one kept to the middle half
 * of it, one kept to the left not at all; the same for the height. The 3D view isn't moved.
 * @param widthChange How much wider the canvas got, in canvas pixels.
 * @param heightChange How much taller the canvas got, in canvas pixels.
 */
void FirstPersonMode::shiftAnchored(int widthChange, int heightChange)
{
	for (Surface *surface : _ownSurfaces)
	{
		if (surface == _view)
		{
			continue;
		}
		auto found = _anchors.find(surface);
		const Anchor anchor = found != _anchors.end() ? found->second : TOP_LEFT;
		surface->setX(surface->getX() + ((anchor == TOP_RIGHT || anchor == RIGHT_CENTER) ? widthChange
			: (anchor == TOP_CENTER || anchor == CENTER) ? widthChange / 2 : 0));
		surface->setY(surface->getY() + (anchor == BOTTOM_LEFT ? heightChange
			: (anchor == CENTER || anchor == RIGHT_CENTER) ? heightChange / 2 : 0));
	}
}

/**
 * Lets one of our buttons see a mouse event (for its pressed look and click sound). Our
 * buttons live on the canvas, so on a magnified screen the event is remade with the mouse at
 * the matching canvas point.
 */
void FirstPersonMode::handleOnCanvas(TextButton *button, Action *action)
{
	// Not magnified: the canvas is the screen.
	if (_uiScale == 1 && !_canvasOnDisplay)
	{
		button->handle(action, _state);
		return;
	}

	// Where the mouse is on the canvas, turned back into window pixels as if the game's screen
	// were the canvas: the Action made from that event then reports the canvas point.
	SDL_Event event = *action->getDetails();
	const double canvasX = toCanvasX(action->getAbsoluteXMouse()), canvasY = toCanvasY(action->getAbsoluteYMouse());
	event.button.x = (Uint16)std::lround(canvasX * action->getXScale() + action->getLeftBlackBand());
	event.button.y = (Uint16)std::lround(canvasY * action->getYScale() + action->getTopBlackBand());
	Action scaledAction(&event, action->getXScale(), action->getYScale(), action->getTopBlackBand(), action->getLeftBlackBand());
	button->handle(&scaledAction, _state);
}

/**
 * Where the game's warnings ("not enough time units"...) go in full 3D: just above the
 * (magnified) info panel, in screen pixels.
 */
int FirstPersonMode::warningY() const
{
	return Options::baseYResolution - toScreenY(PanelH) - 28;
}

/**
 * Adds a small button to the top bar, in the style of the original UI (colours from the mod's
 * "firstPerson" interface).
 * @param x Its left edge, in canvas pixels.
 * @param width Its width, in canvas pixels.
 * @param label The text on it (already translated).
 */
TextButton *FirstPersonMode::makeButton(int x, int width, const std::string &label)
{
	TextButton *button = new TextButton(width, BarH, x, BarY);
	_state->add(button, "button", "firstPerson");
	button->setSmall();
	button->setText(label);
	_ownSurfaces.insert(button);
	_topBarSurfaces.insert(button);
	return button;
}

/**
 * How wide a button must be for its label in the small font: the text's width, measured with a
 * throwaway Text, plus 3 pixels of margin on each side.
 */
int FirstPersonMode::labelWidth(const std::string &label) const
{
	Text measure(200, 9, 0, 0);
	measure.initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
	measure.setSmall();
	measure.setText(label);
	return measure.getTextWidth() + 6;
}

/**
 * Adds a text with a dark shadow one pixel down and right of it, so it reads over the 3D view.
 * The shadow is added right before the text (syncShadows relies on that).
 * @param width The text's width, in canvas pixels.
 * @param height The text's height, in canvas pixels.
 * @param x The text's left edge, in canvas pixels.
 * @param y The text's top edge, in canvas pixels.
 * @param id The element of the mod's interface that colours the text.
 * @param category The mod's interface the element is in ("firstPerson", "battlescape"...).
 * @return The text (the shadow is kept in _textShadows).
 */
Text *FirstPersonMode::addText(int width, int height, int x, int y, const std::string &id, const std::string &category)
{
	Text *shadow = new Text(width, height, x + 1, y + 1);
	Text *text = new Text(width, height, x, y);
	_state->add(shadow);
	_state->add(text, id, category);
	shadow->setColor(10); // glyph shades 1..5 land on 11..15: dark greys, inside the group
	text->setHighContrast(true);
	_textShadows.push_back(shadow);
	_ownSurfaces.insert(shadow);
	_ownSurfaces.insert(text);
	return text;
}

/**
 * Adds a number (like the original HUD's stats) with a dark shadow one pixel down and right of
 * it, added right before it (syncShadows relies on that).
 * @param x The number's left edge, in canvas pixels.
 * @param y The number's top edge, in canvas pixels.
 * @param id The element of the battlescape interface that colours it.
 */
NumberText *FirstPersonMode::addNumber(int x, int y, const std::string &id)
{
	NumberText *shadow = new NumberText(15, 5, x + 1, y + 1);
	NumberText *number = new NumberText(15, 5, x, y);
	_state->add(shadow);
	_state->add(number, id, "battlescape");
	shadow->setColor(10); // dark grey, as for the texts' shadows
	_numberTextShadows.push_back(shadow);
	_ownSurfaces.insert(shadow);
	_ownSurfaces.insert(number);
	return number;
}

/**
 * Copies the place, visibility and text (or number) of every text and number onto its shadow,
 * once per frame after everything was updated. The alignment is copied with a new text.
 */
void FirstPersonMode::syncShadows()
{
	// Texts and their shadows were added in pairs, the shadow right before the text: find the
	// shadows among the state's surfaces, and the surface after each one is its text.
	auto &surfaces = _state->_surfaces;
	for (size_t surfaceIndex = 0; surfaceIndex + 1 < surfaces.size(); ++surfaceIndex)
	{
		if (Text *shadow = dynamic_cast<Text*>(surfaces[surfaceIndex]))
		{
			// A text's shadow: follow the text, and take its words only when they changed (setting
			// a text redraws it).
			if (std::find(_textShadows.begin(), _textShadows.end(), shadow) == _textShadows.end()) continue;
			Text *text = static_cast<Text*>(surfaces[surfaceIndex + 1]);
			shadow->setX(text->getX() + 1);
			shadow->setY(text->getY() + 1);
			shadow->setVisible(text->getVisible());
			if (shadow->getText() != text->getText())
			{
				shadow->setAlign(text->getAlign());
				shadow->setText(text->getText());
			}
		}
		else if (NumberText *numberShadow = dynamic_cast<NumberText*>(surfaces[surfaceIndex]))
		{
			// A number's shadow: follow the number and show the same value.
			if (std::find(_numberTextShadows.begin(), _numberTextShadows.end(), numberShadow) == _numberTextShadows.end()) continue;
			NumberText *number = static_cast<NumberText*>(surfaces[surfaceIndex + 1]);
			numberShadow->setX(number->getX() + 1);
			numberShadow->setY(number->getY() + 1);
			numberShadow->setVisible(number->getVisible());
			numberShadow->setValue(number->getValue());
		}
	}
}

/**
 * Cleans up. The surfaces were added to the state, which deletes them; only what we made
 * outside it is deleted here. The isometric map gets its own sound listener and night vision
 * key back.
 */
FirstPersonMode::~FirstPersonMode()
{
	// VR's calls (see FirstPersonVr.h) no longer come here; the HUD's own picture goes.
	if (_vrCurrent == this)
	{
		_vrCurrent = nullptr;
	}
	delete _vrHud;
	Timer::maxFrameSkip = Options::maxFrameSkip; // the player's own frame skipping again (see catchUp)
	_state->_map->setSoundListener(nullptr);
	_state->_map->setNightVisionHoldBlocked(false);
	_state->_map->setSelectorTransform(nullptr); // (3DIso research: its picture goes with us)
	_state->_map->getCamera()->setScrollTransform(nullptr);
	_state->_map->setOverlayTarget(nullptr);
	FirstPerson::setIsoFullColour(false);
	delete _isoOverlay;
	_isoOverlay = nullptr;

	// A quickload deletes the battle before this screen goes; only a battle that lives on (the
	// screen remade after an options change) gets its pathfinding back to the rules (no vault
	// let through).
	SavedGame *savedGame = _game->getSavedGame();
	const bool battleLivesOn = savedGame && savedGame->getSavedBattle() == _save;
	if (battleLivesOn)
	{
		_save->getPathfinding()->setVault(nullptr, {});
	}
	// The battle is over for this screen, unless a remade screen took it over (an options
	// change; a quickload has already noted its own new battle) or it goes on to its next stage
	// (a two-stage mission: the mission type changed, the choices carry on, the fuses don't):
	// forget the choices, so a new battle, which may get the same address, starts afresh (Start
	// battles in 3D, the flashlight on), however soon.
	const bool nextStage = battleLivesOn && _save->getMissionType() != _missionAtStart;
	if (nextStage)
	{
		lastDoomFuses.clear();
	}
	else if (lastBattle == _save && (!battleLivesOn || _save->getBattleState() == _state))
	{
		lastBattle = nullptr;
		lastThinkTicks = 0;
	}

	// What isn't owned by the state.
	delete _heldSprite; // not added to the state
	delete _offHandSprite;
	delete _bodySurface;
	delete _scanner;
	delete _ambience;
	delete _sounds;
	delete _canvas;
	delete _composed;
}

/**
 * Is the 3D view showing (in place of the isometric map)?
 */
bool FirstPersonMode::isActive() const
{
	return _view->getVisible();
}

/**
 * Does the aliens' turn play in 3D, the view following them (the setting "Alien turn in 3D")?
 * Never with VR mode on (the owner: "In vr mode alien turn is in iso mode not 3d ... as too much
 * switching of camera"): the isometric map shows it, and 3D comes back with the player's turn.
 * @return True if so.
 */
bool FirstPersonMode::alienTurn3D() const
{
	return Options::fpsAlienTurn3D && !vrOn();
}

/**
 * Switches between the isometric map and the 3D view (the 3D key or button), as the ISO/3D
 * button says: the map when the 3D view shows, else 3D. The switch itself happens in think(),
 * called right away, which also checks there is a soldier to look through. In the aliens' turn
 * with "Alien turn in 3D" off the map shows while 3D is wanted: 3D then comes with the player's
 * turn, and a line says so.
 */
void FirstPersonMode::toggleView()
{
	// With the Live command feed the feed (the 3D view in its window) and the full 3D view take turns; from the
	// isometric map (the aliens' turn on it) the key goes to the feed.
	if (Options::fpsCommandFeed && !vrOn())
	{
		_feedPreferred = !_feedPreferred || !isActive();
		_wants3D = true;
		rememberChoices();
		think();
		return;
	}
	_wants3D = _isoFlight.on ? !_isoFlight.to3D : !isActive(); // (3DIso research: pressed during a flight, it turns back)
	if (_wants3D && _save->getSide() != FACTION_PLAYER && !alienTurn3D())
	{
		_state->warning("STR_FPS_3D_NEXT_TURN");
	}
	rememberChoices();
	think();
}

/**
 * Shows or hides the 3D view in place of the isometric map, and sets up everything that works
 * differently in 3D: flight costs and vaults (back to the rules on the map), the night vision
 * hold key, where sounds are heard from, the mouse, and where the game's warnings appear.
 * @param active True to show the 3D view, false to go back to the isometric map.
 */
void FirstPersonMode::setActive(bool active)
{
	// Back on the isometric map, moves are paid for by the rules there: no flight cost of our
	// own (the setting "Flight cost per level"), no vault in progress. And the map's camera goes
	// to the soldier the view looked through (on their level), who may have walked far from
	// where the map was left, so nobody gets lost.
	if (!active)
	{
		_save->getPathfinding()->setFlightCost(nullptr, -1); // by the rules on the isometric map
		endVault();
		BattleUnit *viewedUnit = _save->getSide() == FACTION_PLAYER ? _save->getSelectedUnit() : _lastPlayerUnit;
		if (viewedUnit && !viewedUnit->isOut() && !feedWanted()) // (the Live command feed's drone stays where it was flown)
		{
			_state->_map->getCamera()->centerOnPosition(viewedUnit->getPosition());
		}
	}
	// A path the isometric map previewed (its arrows) belongs to before the switch: whichever
	// way it goes, the soldier may have moved since, so the old arrows go.
	_save->getPathfinding()->removePreview();

	// In 3D the night vision hold key (Space) jumps; night vision there is gear with its own key.
	_state->_map->setNightVisionHoldBlocked(active && Options::keyFpsJump == Options::keyNightVisionHold);

	// Going into 3D, and back, the map forgets any arrow key it was scrolling with: a key let go
	// in 3D, where the map doesn't get it, would leave it drifting. (Done both ways, as the map
	// takes the release only while its cursor shows, which isn't so while the game is busy.)
	for (SDL_Keycode scrollKey : { Options::keyBattleLeft, Options::keyBattleRight, Options::keyBattleUp, Options::keyBattleDown })
	{
		SDL_Event release = {};
		release.type = SDL_KEYUP;
		release.key.keysym.sym = scrollKey;
		Action releaseAction(&release, 1.0, 1.0, 0, 0);
		_state->_map->getCamera()->keyboardRelease(&releaseAction, _state);
	}
	_state->_map->getCamera()->stopKeyScrolling();

	// In 3D, sounds are panned for the soldier's ears (where the view looks), not the map's camera.
	if (active)
	{
		_state->_map->setSoundListener([this](const Position &tile) { return _view->soundAngle(tile); });
	}
	else
	{
		_state->_map->setSoundListener(nullptr);
	}

	// Swap the view and the map. Mouse look starts on unless the classic HUD wants the cursor,
	// and the camera is placed on the unit afresh.
	_view->setVisible(active);
	_state->_map->setVisible(!active || feedWanted()); // (the Live command feed shows the map as its drone's feed)
	_state->_map->invalidate();
	_dragLook = false;
	_mouseLook = active && !_classicHud && !feedWanted(); // (in the feed's 3D window a click gives it the mouse)
	_trackedUnit = nullptr; // re-place the camera on the unit
	relayoutIfNeeded(); // (with the view at the display's size the overlays are laid out for the display in 3D, for the screen over the map)

	// Keep warnings ("not enough time units"...) visible above the panel when the classic HUD is hidden.
	_state->_warning->setY(active && !_classicHud && !feedWanted() ? warningY() : _warningY);
}

/**
 * Shows or hides the original bottom HUD over the 3D view. Showing it frees the mouse so its
 * buttons (fire modes, kneel, ...) can be clicked; hiding it goes back to mouse look. The
 * game's warnings go back to their place above the HUD, or above our info panel without it.
 * @param on True to show the classic HUD.
 */
void FirstPersonMode::setClassicHud(bool on)
{
	_classicHud = on;
	rememberChoices();
	_dragLook = false;
	_mouseLook = !on;
	_state->_warning->setY(!on ? warningY() : _warningY);
}

/**
 * Steps the 3D HUD round (the hide HUD key, P by default): shown; hidden, for a clear view: the
 * top bar, the info panel, the compass, the lift arrows, the enemy markers and the original's
 * enemy buttons (the crosshair stays); and clean: the crosshair too, with the shot info, the
 * costs, the accuracy line and the name of the unit pointed at (a player's report, GitHub issue
 * #37: "full hudless mode would be useful for a project"); then shown again. The weapon in hand
 * and the trajectory keep their own settings, and the banners and the game's messages still
 * show. Hidden buttons can't be clicked. On the isometric map the top bar shows as ever. Each
 * hidden step's message says which key goes on.
 */
void FirstPersonMode::toggleHud()
{
	if (!_hudHidden)
	{
		_hudHidden = true;
		_state->warningRaw(_state->tr("STR_FPS_HUD_HIDDEN").arg(keyLabel(Options::keyFpsHideHud)));
	}
	else if (!_hudClean)
	{
		_hudClean = true;
		_state->warningRaw(_state->tr("STR_FPS_HUD_CLEAN").arg(keyLabel(Options::keyFpsHideHud)));
	}
	else
	{
		_hudHidden = _hudClean = false;
	}
	rememberChoices();
}

/**
 * The 3D HUD key on the isometric map (P by default): the map camera off, or on again at the
 * size it last had (medium the first time), as if set so in the settings (and kept with them).
 * A message says which key does the opposite.
 */
void FirstPersonMode::toggleMapCamera()
{
	if (Options::fpsMapCamera > 0)
	{
		lastMapCameraSize = Options::fpsMapCamera;
		Options::fpsMapCamera = 0;
		_state->warningRaw(_state->tr("STR_FPS_MAP_CAMERA_HIDDEN").arg(keyLabel(Options::keyFpsHideHud)));
	}
	else
	{
		Options::fpsMapCamera = std::clamp(lastMapCameraSize, 1, 3);
		_state->warningRaw(_state->tr("STR_FPS_MAP_CAMERA_SHOWN").arg(keyLabel(Options::keyFpsHideHud)));
	}
}

/**
 * Switches the flashlight on or off (its key, F by default), with a click of the switch. It's
 * the 3D view's own light (a light on the weapon: a spot on the crosshair at any time of day) and changes nothing in the game's rules;
 * the suit light (L) is the game's personal light and stays separate. Each battle starts with
 * it on.
 */
void FirstPersonMode::toggleFlashlight()
{
	_flashlightOn = !_flashlightOn;
	rememberChoices();
	_sounds->play(FirstPersonSounds::LIGHT_SWITCH);
}

/**
 * Switches sprinting on or off (its key, Caps Lock by default). While it's on, each step in 3D
 * is the game's own run (Ctrl on the map), at a faster pace: by the default rules it costs
 * three quarters of a walk's time units and half as much energy again, and only an armor that
 * allows running runs. A soldier without the energy for a run walks the step instead (see
 * runsStep). Sprinting walks on past enemies coming into sight, as en passant does (GitHub issue
 * #118; on the map a run does so where the mod's rules say so); being shot at stops it. "SPRINT"
 * shows under the compass while it's on. Each battle starts with it off.
 */
void FirstPersonMode::toggleSprint()
{
	_sprintOn = !_sprintOn;
	_sprintStepRan = true; // (a fresh try: the next step tells whether the soldier can run)
	rememberChoices();
}

/**
 * Switches en passant on or off (its key, the one left of 1 by default): while it's on, an enemy
 * coming into sight doesn't stop the walk, as moving with Shift held doesn't on the map (the
 * game's own rule, its "ignore spotted enemies"); being shot at still does, as on the map. The
 * banner still says ENEMY SPOTTED. Sprinting walks on the same way. "EN PASSANT" shows under the
 * compass while it's on. Each battle starts with it off.
 */
void FirstPersonMode::toggleEnPassant()
{
	_enPassantOn = !_enPassantOn;
	rememberChoices();
}

/**
 * The modifier bits of the lean key (setting "Lean") while it's held, when it is a modifier key
 * (Shift, Ctrl or Alt; Left Shift by default). None with leaning off, another lean key, or the
 * key not held.
 */
SDL_Keymod FirstPersonMode::leanModifierHeld() const
{
	if (Options::fpsLean == 0)
	{
		return KMOD_NONE;
	}
	// Which modifier bits the lean key sets.
	int bits = KMOD_NONE;
	switch (Options::keyFpsLeanHold)
	{
	case SDLK_LSHIFT: bits = KMOD_LSHIFT; break;
	case SDLK_RSHIFT: bits = KMOD_RSHIFT; break;
	case SDLK_LCTRL: bits = KMOD_LCTRL; break;
	case SDLK_RCTRL: bits = KMOD_RCTRL; break;
	case SDLK_LALT: bits = KMOD_LALT; break;
	case SDLK_RALT: bits = KMOD_RALT; break;
	default: return KMOD_NONE;
	}
	return (SDL_GetModState() & bits) ? (SDL_Keymod)bits : KMOD_NONE;
}

/**
 * Is a Ctrl or Alt held that is the game's (for a shortcut such as Ctrl+S): then the keys are the
 * game's. A Ctrl or Alt held as the lean key is ours.
 */
bool FirstPersonMode::gameModifierHeld() const
{
	return (SDL_GetModState() & (KMOD_CTRL | KMOD_ALT) & ~leanModifierHeld()) != 0;
}

/**
 * Is the 3D HUD hidden right now: in the 3D view, with the hide HUD key?
 */
bool FirstPersonMode::hudHidden() const
{
	return _hudHidden && isActive();
}

/**
 * Is the view clean right now: in the 3D view, the HUD hidden and the crosshair with it (the
 * hide HUD key's third step, see toggleHud)?
 */
bool FirstPersonMode::hudClean() const
{
	return _hudHidden && _hudClean && isActive();
}

/**
 * Switches between mouse look (the mouse turns the view, captured) and a free cursor (for
 * clicking our buttons and aiming at a point). Ends any right-button drag look.
 * @param on True for mouse look.
 */
void FirstPersonMode::setMouseLook(bool on)
{
	_mouseLook = on;
	_dragLook = false;
	_zoomButtonHeld = false;
}

/**
 * Should the mouse be captured right now? Yes while the 3D view shows and the mouse turns the
 * view: in mouse look, or while the right button drags the view round with a free cursor.
 */
bool FirstPersonMode::wantsMouseCapture() const
{
	return isActive() && (_mouseLook || _dragLook);
}

/**
 * Can the player act right now: their turn, any breakdowns dealt with, the game not busy with
 * an action, no shot in flight, and no popup up?
 */
bool FirstPersonMode::canAct() const
{
	return _state->allowButtons() && _state->_popups.empty();
}

/**
 * Is a shot under way: one of ours handed to the game and still going (its burst, its hits, see
 * endShotAim), or anyone's round in flight or blast going off?
 * @return True if so.
 */
bool FirstPersonMode::shotUnderWay() const
{
	return _shotAimLeft || _state->_map->getProjectile() || !_state->_map->getExplosions()->empty();
}

/**
 * Shots and explosions in 3D at the isometric map's pace: while the 3D view shows and a shot is
 * flying or something is going off (a round's hit, a blast), and for CatchUpGrace after (the
 * step that fires a burst's next round), the battle's timer may run up to CatchUpSteps steps it
 * missed during a frame (OXCE's own frame skipping, whose option is off by default); otherwise
 * the player's own setting. A long chain of terrain explosions runs a step a millisecond from its
 * seventh link on (OXCE's speed-up), which the map keeps up with between its quick frames, a link
 * in about 45 ms; in 3D each step waited for a whole frame (about 29 ms at 1280x800), and a link
 * took 374 ms (a player's report: a building-sized chemical tower took ages to go up). Likewise
 * each round of a burst takes the game about four steps (its flight, its hit, the next round),
 * and with slow frames (a 640x400 view drawn on one core: 36 ms) a 10-round burst of fast rounds
 * took 1.2 s, the map 0.6 s (a player's report on 1.03: the X-Com Files' turbo laser). Walking
 * and everything else the view follows frame by frame keep one step a frame. The timer reads the
 * limit when it next runs (the battle's think runs it before this), so it applies a frame on.
 * Called every frame.
 */
void FirstPersonMode::catchUp()
{
	const Uint32 now = SDL_GetTicks();
	if (isActive() && (_state->_map->getProjectile() || !_state->_map->getExplosions()->empty()))
	{
		_catchUpUntil = now + CatchUpGrace;
	}
	const bool catchingUp = isActive() && now < _catchUpUntil;
	Timer::maxFrameSkip = catchingUp ? std::max(Options::maxFrameSkip, CatchUpSteps) : Options::maxFrameSkip;
}

/**
 * Is doom mode on? It is with its own setting, and with any of its extras, which are doom mode
 * plus more: IDKFA (endless ammo), IDDQD (invincible soldiers) and IDCLIP (noclip).
 * @return True if doom mode is on.
 */
bool FirstPersonMode::doomModeOn()
{
	return Options::fpsDoomMode || Options::fpsDoomIdkfa || Options::fpsDoomIddqd || Options::fpsDoomIdclip;
}

/**
 * The doom mode key (Insert): with any of the cheats on (doom mode, IDKFA, IDDQD, IDCLIP), switches them
 * all off and remembers which were on; with none on, switches those back on (doom mode alone if
 * there are none to remember). The game's warning line says which.
 */
void FirstPersonMode::toggleCheats()
{
	if (doomModeOn())
	{
		_pausedCheats = (Options::fpsDoomMode ? 1 : 0) | (Options::fpsDoomIdkfa ? 2 : 0) | (Options::fpsDoomIddqd ? 4 : 0) | (Options::fpsDoomIdclip ? 8 : 0);
		Options::fpsDoomMode = Options::fpsDoomIdkfa = Options::fpsDoomIddqd = Options::fpsDoomIdclip = false;
	}
	else
	{
		const int cheats = _pausedCheats ? _pausedCheats : 1;
		Options::fpsDoomMode = (cheats & 1) != 0;
		Options::fpsDoomIdkfa = (cheats & 2) != 0;
		Options::fpsDoomIddqd = (cheats & 4) != 0;
		Options::fpsDoomIdclip = (cheats & 8) != 0;
	}
	// A line says which (the panel's tag hides with the classic HUD or the 3D HUD hidden), and
	// the options file keeps it, so the options screen's Cancel doesn't quietly undo it.
	if (doomModeOn()) _state->warningRaw(_state->tr("STR_FPS_CHEATS_ON").arg(cheatTag()));
	else _state->warning("STR_FPS_CHEATS_OFF");
	rememberChoices();
	Options::save();
}

/**
 * The cheats that are on, as the info panel tags them: DOOM MODE, or the extras that are on
 * (IDDQD, IDKFA, IDCLIP).
 */
std::string FirstPersonMode::cheatTag() const
{
	if (!Options::fpsDoomIddqd && !Options::fpsDoomIdkfa && !Options::fpsDoomIdclip)
	{
		return std::string(_state->tr("STR_FPS_DOOM"));
	}
	std::string tag;
	auto add = [&](const char *id) { tag += (tag.empty() ? "" : " ") + std::string(_state->tr(id)); };
	if (Options::fpsDoomIddqd) add("STR_FPS_IDDQD");
	if (Options::fpsDoomIdkfa) add("STR_FPS_IDKFA");
	if (Options::fpsDoomIdclip) add("STR_FPS_IDCLIP");
	return tag;
}

/**
 * Doom mode (debug): the selected soldier never runs out of time units or energy. Called every
 * frame: tops them up. Ammo still counts, unless IDKFA is on too: then every clip loaded in the
 * weapons in both hands is filled up again, and no shot spends a round (see endlessAmmo, which
 * keeps a one-round clip such as a rocket from being used up).
 */
void FirstPersonMode::applyDoomMode(BattleUnit *unit)
{
	unit->resetTimeUnitsAndEnergy();
	if (!Options::fpsDoomIdkfa)
	{
		return;
	}
	for (BattleItem *item : { unit->getRightHandWeapon(), unit->getLeftHandWeapon() })
	{
		if (!item)
		{
			continue;
		}
		// Every ammo slot of the weapon: a loaded clip is filled up again (items without a clip
		// size, like melee weapons, are left alone).
		for (int slot = 0; slot < RuleItem::AmmoSlotMax; ++slot)
		{
			BattleItem *ammo = item->getAmmoForSlot(slot);
			if (ammo && ammo->getRules()->getClipSize() > 0)
			{
				ammo->setAmmoQuantity(ammo->getRules()->getClipSize());
			}
		}
	}
}

/**
 * Doom IDDQD: X-COM's units take no damage (see invincible), and this keeps their morale full,
 * so seeing a squadmate fall doesn't make them panic either. Called every frame of the battle,
 * on either side's turn.
 */
void FirstPersonMode::applyGodMode()
{
	if (!Options::fpsDoomIddqd)
	{
		return;
	}
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getFaction() == FACTION_PLAYER && !unit->isOut() && unit->getMorale() < 100)
		{
			unit->moraleChange(100 - unit->getMorale());
		}
	}
}

/**
 * Doom IDKFA, asked by the game whenever a weapon spends ammo: does this unit's shot spend
 * nothing? The selected soldier's, on the player's turn, in the 3D view (where doom mode tops
 * the soldier up), with IDKFA on.
 * @param state The battlescape screen.
 * @param unit The unit holding the weapon.
 * @return True if the shot spends no ammo.
 */
bool FirstPersonMode::endlessAmmo(BattlescapeState *state, const BattleUnit *unit)
{
	return Options::fpsDoomIdkfa && state && state->_firstPerson && state->_firstPerson->isActive() && unit
		&& unit->getFaction() == FACTION_PLAYER && state->_firstPerson->_save->getSide() == FACTION_PLAYER
		&& state->_firstPerson->_save->getSelectedUnit() == unit;
}

/**
 * Doom IDDQD, asked by the game whenever a unit is about to take damage: is it invincible? Any
 * of X-COM's units, on either side's turn and in either view, with IDDQD on (the alien turn is
 * when they get shot).
 * @param state The battlescape screen.
 * @param unit The unit about to be hurt.
 * @return True if it takes no damage.
 */
bool FirstPersonMode::invincible(BattlescapeState *state, const BattleUnit *unit)
{
	return Options::fpsDoomIddqd && state && state->_firstPerson && unit && unit->getFaction() == FACTION_PLAYER;
}

/**
 * Doom IDCLIP, asked by the game whenever a unit with no floor under it is about to fall: does it
 * float where it is instead? Any of X-COM's units, in either view, with IDCLIP on: a soldier
 * walked out into the air or up through a ceiling stays there (see noclipMove), and falls only
 * once IDCLIP is switched off (see applyNoclipEnd).
 * @param state The battlescape screen.
 * @param unit The unit about to fall.
 * @return True if it doesn't fall.
 */
bool FirstPersonMode::noclipFloats(BattlescapeState *state, const BattleUnit *unit)
{
	return Options::fpsDoomIdclip && state && state->_firstPerson && unit && unit->getFaction() == FACTION_PLAYER;
}

/**
 * Doom IDCLIP (a player's request, GitHub issue #45: "turns movement hit detection off ... If
 * inside terrain when on, grant ghost property to current square to assist visibility. Enable fly
 * ... so player doesn't fall into terrain"): does this soldier walk and float through anything
 * right now? The player's own unit, in the 3D view, with IDCLIP on. (A unit two tiles by two as well: it kept the
 * rules at first, and a player, GitHub issue #234: "2x2 units dont idclip".)
 * @param unit The soldier looked through.
 * @return True if their moves go through walls, doors and floors.
 */
bool FirstPersonMode::noclipOn(const BattleUnit *unit) const
{
	return Options::fpsDoomIdclip && isActive() && unit && unit->getFaction() == FACTION_PLAYER && !unit->isOut();
}

/**
 * Doom IDCLIP: is there room for a unit at a place? Every tile it would stand on (four, for one two tiles by two) is
 * on the map, with no other unit on it. Walls, objects, floors and the air don't count.
 * @param unit The unit.
 * @param target Where it would stand (its tile; a big unit's north-western one).
 * @return True if it may be put there.
 */
bool FirstPersonMode::noclipRoom(const BattleUnit *unit, const Position &target) const
{
	const int size = unit->getArmor()->getSize();
	for (int x = 0; x < size; ++x)
	{
		for (int y = 0; y < size; ++y)
		{
			const Tile *tile = _save->getTile(target + Position(x, y, 0));
			if (!tile || (tile->getUnit() && tile->getUnit() != unit)) return false;
		}
	}
	return true;
}

/**
 * Doom IDCLIP switched off (by its setting or the doom mode key), called every frame of the
 * battle: X-COM's units it left in the air (or inside the ground's level, with no floor) get the
 * game's gravity, so those who can't fly fall and flyers take to the air, as after a floor gives
 * way. The game starts the falls itself as soon as nothing else is going on.
 */
void FirstPersonMode::applyNoclipEnd()
{
	if (_idclipWas && !Options::fpsDoomIdclip)
	{
		for (BattleUnit *unit : *_save->getUnits())
		{
			if (unit->getFaction() == FACTION_PLAYER && !unit->isOut() && unit->getTile())
			{
				_save->getTileEngine()->applyGravity(unit->getTile());
			}
		}
	}
	_idclipWas = Options::fpsDoomIdclip;
}

/**
 * Doom mode with real-time grenades (a first step towards a real-time mode): a grenade armed
 * there burns for its fuse (the setting "Doom grenade fuse" when it was armed) and then goes off
 * wherever it is: where it lies, where it lands if it's still in the air, or in the hand of
 * whoever holds it. Primed on throw (the default, see fire) it starts burning the moment it
 * leaves the hand, and one whose throw the game refused stays in the hand, armed and not
 * burning, until it does; with its pin pulled by PRIME it burns from the click, in the hand too. The explosion is
 * the game's own, as its end-of-turn fuse makes it (an ExplosionBState for a timed grenade at
 * the grenade's tile, which also removes it). One at a time, on the player's turn, when the game
 * isn't busy with a shot or another explosion. Leaving Doom mode stops the real-time fuses (the
 * game's own fuse, set to 0, is still there: the grenade then goes off at the end of the turn,
 * as usual).
 * @param now The time, SDL ticks.
 */
void FirstPersonMode::updateDoomFuses(Uint32 now)
{
	if (_doomFuses.empty())
	{
		return;
	}
	if (!doomModeOn())
	{
		_doomFuses.clear();
		return;
	}
	// A grenade (by its id), if it's still in the battle.
	auto findGrenade = [this](int id) -> BattleItem*
	{
		for (BattleItem *item : *_save->getItems())
		{
			if (item->getId() == id) return item;
		}
		return nullptr;
	};

	// A grenade armed to burn from leaving the hand starts burning the moment it does, even while
	// the game is busy flying it (a thrown item has no owner from the throw on).
	for (auto &entry : _doomFuses)
	{
		if (entry.second.lit) continue;
		const BattleItem *grenade = findGrenade(entry.first);
		if (grenade && !grenade->getOwner()) entry.second.lit = std::max<Uint32>(1, now);
	}

	BattlescapeGame *battleGame = _state->_battleGame;
	if (battleGame->isBusy() || _save->getSide() != FACTION_PLAYER)
	{
		return;
	}
	for (auto entry = _doomFuses.begin(); entry != _doomFuses.end(); )
	{
		// The grenade, if it's still in the battle and still primed.
		BattleItem *grenade = findGrenade(entry->first);
		if (!grenade || grenade->getFuseTimer() < 0)
		{
			entry = _doomFuses.erase(entry);
			continue;
		}
		// Still in a hand: not burning yet.
		DoomFuse &fuse = entry->second;
		if (!fuse.lit)
		{
			++entry;
			continue;
		}
		if (now - fuse.lit < fuse.length)
		{
			++entry;
			continue;
		}

		// Where it is: on the ground, or (picked up again meanwhile) with whoever carries it, at
		// their middle. Neither: it's in the air, and goes off where it lands.
		const Tile *tile = grenade->getTile();
		BattleUnit *carrier = grenade->getOwner();
		if (!tile && carrier)
		{
			tile = carrier->getTile();
		}
		if (!tile)
		{
			++entry;
			continue;
		}
		const Position centre = tile->getPosition().toVoxel() + Position(8, 8, -tile->getTerrainLevel() + (carrier ? carrier->getHeight() / 2 : 0));
		_doomFuses.erase(entry);
		battleGame->statePushBack(new ExplosionBState(battleGame, centre, BattleActionAttack::GetBeforeShoot(BA_TRIGGER_TIMED_GRENADE, carrier, grenade)));
		return; // the next one waits for this explosion to end
	}
}

/**
 * Combat music (setting "Combat music"): while aliens are in sight a tenser track plays (the
 * first of the mod's combat tracks that exists); when none has been seen for 8 seconds the
 * battle music comes back. With the setting off, the battle music comes back at once.
 * @param now The current time (SDL ticks, ms).
 */
void FirstPersonMode::updateMusic(Uint32 now)
{
	// The battle's own music: the mission's track, or the standard tactical one.
	Mod *mod = _game->getMod();
	auto playBattleMusic = [&]() { mod->playMusic(_save->getMusic().empty() ? std::string("GMTACTIC") : _save->getMusic()); };

	// Setting off: back to the battle music if the combat track was playing.
	if (!Options::fpsCombatMusic)
	{
		if (_musicTense)
		{
			_musicTense = false;
			playBattleMusic();
		}
		return;
	}

	// Is any hostile unit, still standing, in sight right now?
	bool enemyInSight = false;
	for (const BattleUnit *unit : *_save->getUnits())
	{
		if (!unit->isOut() && unit->getFaction() == FACTION_HOSTILE && unit->getVisible())
		{
			enemyInSight = true;
			break;
		}
	}
	if (enemyInSight)
	{
		_lastEnemySeen = now;
	}

	// An enemy came into sight: the first combat track the mod really has starts. None seen
	// for 8 seconds: the battle music again.
	if (enemyInSight && !_musicTense)
	{
		for (const std::string &track : mod->getFirstPerson().getCombatMusic())
		{
			if (mod->getMusic(track, false))
			{
				mod->playMusic(track);
				_musicTense = true;
				break;
			}
		}
	}
	else if (!enemyInSight && _musicTense && now - _lastEnemySeen > 8000)
	{
		_musicTense = false;
		playBattleMusic();
	}
}

/**
 * The per-frame update, as a sequence of named steps. On your turn the camera is the selected
 * soldier, who moves, turns and fires; on the aliens' turn it stays with your last soldier (look
 * around only, setting "Alien turn in 3D") and shows whatever that soldier's squad can see.
 * While a soldier goes down or breaks down, the death or breakdown camera takes the frame over.
 */
/**
 * Setting "Voxel units": every unit's body is made from the battle's start, on the map too, not only those the 3D view
 * has seen (the owner: the wait must not come at the first click of the 3D button; and with "Realistic hitboxes" a
 * shot hits a unit where its body is, see FirstPerson::spriteHit): each frame, those still without a body for how they
 * stand now have their frames gathered, for VoxelBodiesMs at most (see VoxelUnits::modelFor; the builder's threads make
 * the bodies, the units in the battle's order: the player's first). A unit without one yet is drawn, and hit, as its
 * picture. (By the time it takes, not by a count: a hundred units alike used to be gathered in one frame, see modelFor.)
 * @param milliseconds The time frames are gathered for at most (a frame's: VoxelBodiesMs; where a battle begins, more:
 *   see primeView).
 */
void FirstPersonMode::makeVoxelBodies(double milliseconds)
{
	if (!_bodySurface)
	{
		_bodySurface = new Surface(VoxelBodies::FrameSize, VoxelBodies::FrameSize);
	}
	UnitSprite unitSprite(_bodySurface, _game->getMod(), _save, _save->getAnimFrame(), _save->getDepth() != 0, 0, 0);
	const auto began = std::chrono::steady_clock::now();
	for (BattleUnit *unit : *_save->getUnits())
	{
		int builds = 1;
		// (a unit down already has one made too, once: its corpse lies as its body, as one that went down in view)
		if (VoxelUnits::suitsDown(unit) ? _voxelUnits->lastBody(unit) != nullptr : !VoxelUnits::suits(unit)) continue;
		if (unit->isOutThresholdExceed() && _voxelUnits->lastBody(unit)) continue; // (struck down, about to fall: it keeps the body it had, see drawUnits)
		if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING) continue;
		int walkPhase;
		_voxelUnits->modelFor(unit, unitSprite, _game->getMod()->getSurfaceSet("BIGOBS.PCK"), _game->getMod()->getSurfaceSet("FLOOROB.PCK"), _save,
			_state->getPalette(), builds, walkPhase);
		// (its frames were gathered: is there time for another's?)
		if (builds == 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() >= milliseconds) break;
	}
	_bodySurface->clear();
}

/**
 * Says that the 3D view is being prepared, on a screen of its own shown at once: two lines in the middle of a black
 * screen, in the battle's palette (the screen before still showed, in a palette that is no longer the display's).
 * The game's next frame draws over it.
 */
void FirstPersonMode::showPreparing(int percent)
{
	SDL_Surface *screen = _game->getScreen()->getSurface();
	const int width = Options::baseXResolution, height = Options::baseYResolution;
	_game->getScreen()->clear();
	const SDL_Color *palette = _state->getPalette();
	Text title(width, 17, 0, height / 2 - 14), note(width, 9, 0, height / 2 + 6), done(width, 9, 0, height / 2 + 27);
	for (Text *text : { &title, &note, &done })
	{
		text->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
		text->setPalette(palette);
		text->setAlign(ALIGN_CENTER);
	}
	title.setBig();
	title.setColor(textColor(palette, 110, 170, 255));
	// (the title's dots running while the card gets ready, one, two, three: the driver takes a big program in one go,
	// seconds the first time, and the screen must not look hung meanwhile; its place kept, the full title's)
	std::string titleText = _state->tr("STR_FPS_PREPARING_3D");
	if (percent >= 0)
	{
		title.setText(titleText);
		title.setAlign(ALIGN_LEFT);
		title.setX((width - title.getTextWidth()) / 2);
		while (!titleText.empty() && titleText.back() == '.') titleText.pop_back();
		titleText += std::string(1 + (SDL_GetTicks() / 350) % 3, '.');
	}
	title.setText(titleText);
	note.setColor(textColor(palette, 160, 160, 170));
	note.setText(_state->tr("STR_FPS_PREPARING_3D_NOTE"));
	title.blit(screen);
	note.blit(screen);
	if (percent >= 0)
	{
		// How far the card has got (see FirstPersonView::gpuProgress): a bar filling, and its percent under it.
		const int barWidth = 120, barHeight = 5, barX = (width - barWidth) / 2, barY = height / 2 + 18;
		SDL_Rect frame = { (Sint16)(barX - 1), (Sint16)(barY - 1), (Uint16)(barWidth + 2), (Uint16)(barHeight + 2) };
		SDL_FillRect(screen, &frame, textColor(palette, 160, 160, 170));
		SDL_Rect inside = { (Sint16)barX, (Sint16)barY, (Uint16)barWidth, (Uint16)barHeight };
		SDL_FillRect(screen, &inside, textColor(palette, 20, 24, 40));
		SDL_Rect filled = { (Sint16)barX, (Sint16)barY, (Uint16)(barWidth * std::clamp(percent, 0, 100) / 100), (Uint16)barHeight };
		if (filled.w > 0) SDL_FillRect(screen, &filled, textColor(palette, 110, 170, 255));
		done.setColor(textColor(palette, 160, 160, 170));
		done.setText(std::to_string(std::clamp(percent, 0, 100)) + "%");
		done.blit(screen);
	}
	_game->getScreen()->flip();
}

/**
 * The first 3D frame's waiting, done where a battle begins (the owner: "Can we move shader loading the first time
 * into battle load? (players are confused as game freezes for 2s upon first click on 3D icon in battlescape)"). With
 * the setting "Renderer" on GPU the first frame of the 3D view in a battle is slow: the driver finishes each program
 * the first time something is drawn with it, the pictures the card draws into are made, and the whole map and the
 * units' bodies are sent to the card. So that frame is drawn ahead here, unseen, while the map shows: through the
 * eyes of the soldier the view will open on, the way they face, with the view's own settings, as the first click of
 * the 3D button would draw it; that click then finds everything there.
 * The battle's first frame comes while the screen before it still shows (the loading box, the next turn's screen),
 * so it waits there for a card that is still being opened (PrimeCardWaitMs at most: the driver builds the shaders
 * then), and with "Voxel units" for the bodies (PrimeBodiesMs at most), so their program is finished and they are
 * sent too. Where that can take seconds (the first time in a run of the game, or a card still being opened) the
 * screen says so first (see showPreparing). A card ready only later (the first run on a machine, the shaders built
 * for many seconds; the renderer switched to GPU in a battle) has its frame drawn ahead when it is, if the map still
 * shows. With 3D showing first,
 * the view's own first frame on the card does the same, as before. Not in VR mode, nor on a headset at all (its
 * frames are its shell's).
 * @param unit The soldier 3D would look through now, or null for none.
 */
void FirstPersonMode::primeView(BattleUnit *unit)
{
#ifdef __ANDROID__
	(void)unit; // (a headset's frames, the pane's too, come as its shell asks for them: left as they were)
#else
	if (_viewPrimed || Options::fpsRenderer != 1 || vrOn())
	{
		return;
	}
	const bool first = !_primeAsked;
	_primeAsked = true;
	// The waiting can be long the first time in a run of the game (the owner: "Sometimes 4-5s"): the card opened, the
	// driver finishing its programs. The screen says so then, in place of the one before, which would look hung.
	static bool preparedBefore = false;
	const bool someone = isActive() || (unit && !unit->isOut() && unit->getTile());
	const auto began = std::chrono::steady_clock::now();
	// (3DIso on: everything for it too, behind this screen, the owner's word: its program, built with the rest, can take
	// many seconds the first time; then its first frame, see below)
	const bool isoFirst = Options::fpsIso && !isActive();
	const int waitMs = !first ? 0 : isoFirst ? PrimeIsoCardWaitMs : PrimeCardWaitMs;
	bool ready = false;
	if (first && someone && (FirstPersonView::gpuOpening() || (!preparedBefore && FirstPersonView::gpuReady(0))))
	{
		// The screen, with how far the card has got (the owner: "show progress on preparing 3D"), drawn again a few times a
		// second while it gets ready; the window answers the desktop meanwhile (its events left for the game).
		for (;;)
		{
			ready = FirstPersonView::gpuReady(0);
			showPreparing(ready ? 100 : FirstPersonView::gpuProgress());
			if (ready || !FirstPersonView::gpuOpening()) break;
			if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() >= waitMs) break;
			SDL_PumpEvents();
			ready = FirstPersonView::gpuReady(100);
			if (ready)
			{
				showPreparing(100);
				break;
			}
		}
		ready = FirstPersonView::gpuReady(0);
	}
	else if (someone)
	{
		ready = FirstPersonView::gpuReady(waitMs);
	}
	if (isActive())
	{
		// (3D shows from the battle's start: the view's own frames do it, on the card from the first if it is there in
		// time; nothing is left once the card draws them)
		_viewPrimed = ready;
		preparedBefore = preparedBefore || _viewPrimed;
		return;
	}
	if (!someone || !ready)
	{
		return;
	}
	const double cardWait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
	// The bodies first, where the battle begins: everyone's frames gathered, and the builder's threads waited for.
	const auto bodiesBegan = std::chrono::steady_clock::now();
	if (Options::fpsVoxelUnits && first)
	{
		for (;;)
		{
			const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bodiesBegan).count();
			makeVoxelBodies(std::max(VoxelBodiesMs, PrimeBodiesMs - waited));
			if (_voxelUnits->idle() || waited >= PrimeBodiesMs) break;
			SDL_Delay(2);
		}
		makeVoxelBodies(VoxelBodiesMs); // (takes what the builder made last)
	}
	const double bodiesWait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bodiesBegan).count();
	// The view as the first click will have it: in full colour if 3D is laid into the display then (see think), from
	// the middle of the soldier's tile at their eyes' height, the way they face (see updateCamera, aimHelmetCamera).
	const auto frameBegan = std::chrono::steady_clock::now();
	int width, height;
	_view->setWantsTrueColor(Options::fpsTrueColor && canvasTargetWhen(true, width, height));
	_view->setCameraUnit(unit);
	const Position position = unit->getPosition();
	const int unitSize = unit->getArmor()->getSize();
	_view->setEye(position.x * 16.0 + 8.0 * unitSize, position.y * 16.0 + 8.0 * unitSize, eyeHeight(unit));
	_view->lookAlong(unit->getDirection());
	_view->setShowFacing(false);
	_view->setFieldOfView(fieldOfViewNow(), 1.0);
	_view->setSmoothLighting(Options::fpsSmoothLighting);
	_view->setMaxDistance(FirstPerson::viewDistance(_game->getMod()));
	_view->setNightVision(_state->_map->getNightVisionColor());
	_view->setSmokeSight(_save->getTileEngine()->getMaxViewDistance() * 16.0f, unit->getVisibilityThroughSmoke(), unit->getVisibilityThroughFire());
	_view->setFlashlight(_flashlightOn, true);
	_view->invalidate();
	_view->draw();
	_viewPrimed = true;
	preparedBefore = true;
	_trackedUnit = nullptr; // (the camera is placed afresh when 3D shows)
	Log(LOG_INFO) << "OpenXcom3D: the 3D view's first frame drawn ahead in "
		<< (int)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameBegan).count() << " ms (the card waited for "
		<< (int)cardWait << " ms, the units' bodies " << (int)bodiesWait << " ms)";
	// 3DIso on (research): its first frame drawn on the card here too, unseen, as the battle's first shown frame will draw
	// it: the map's frames sent to the card, its sun map made, its kind of frame drawn ahead; so 3DIso shows on the card from
	// its first frame, the game never standing still for it.
	if (isoFirst)
	{
		const auto isoBegan = std::chrono::steady_clock::now();
		updateIsoView();
		if (_isoView && _isoView->getVisible())
		{
			Map *map = _state->_map;
			map->invalidate();
			map->draw(); // (its overlays, as blit has them first)
			_isoView->draw();
			Log(LOG_INFO) << "OpenXcom3D: 3DIso's first frame drawn ahead in "
				<< (int)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - isoBegan).count() << " ms";
		}
	}
#endif
}


void FirstPersonMode::think()
{
	// The frame's time: now, and the seconds since the last frame (at most a tenth of a second,
	// so a long pause doesn't make one huge step).
	const Uint32 now = SDL_GetTicks();
	lastThinkTicks = now;
	rememberChoices(); // (the fuses burning change all the time)
	// The display follows "True colour" (with the card, at the display's size): opened in full colour
	// when they ask for it, back as it was when they don't (see FirstPerson::wantsFullColorDisplay).
	if (FirstPerson::wantsFullColorDisplay() != _fullColorDisplay)
	{
		_fullColorDisplay = FirstPerson::wantsFullColorDisplay();
		_game->getScreen()->resetDisplay();
	}
	relayoutIfNeeded(); // (the setting "HUD scale" or "3D resolution" changed, or the display)
	// The hit log's pick ends with another soldier selected, or with its soldier on the move (on the map the aim is put
	// down for either: see noteWeaponInHitLog).
	if (_hitLogPick.open && (_save->getSelectedUnit() != _hitLogPick.unit
		|| _hitLogPick.unit->getStatus() == STATUS_WALKING || _hitLogPick.unit->getStatus() == STATUS_FLYING))
	{
		endHitLogPick();
	}
	_view->setWantsTrueColor(Options::fpsTrueColor && Options::fpsRenderer == 1 && _canvasOnDisplay); // (shown only where the mode lays the view into the display)
	const double elapsedSeconds = _lastFrameTick ? std::min<Uint32>(100, now - _lastFrameTick) / 1000.0 : 0.0;

	// Another screen was on top (popup, next turn, inventory): it had the mouse released.
	const bool resumed = _lastFrameTick && now - _lastFrameTick > 250;
	_lastFrameTick = now;

	// VR mode: the head looks (the shell's, or the desktop's stand-in), first, for everything below that reads where the
	// view looks; and nothing shakes the view (see vrBeginFrame). With it off, nothing.
	vrBeginFrame(elapsedSeconds);
	// The rain, the wind and the fires near the soldier (setting "Ambient sounds"): heard in
	// the 3D view, fading out on the map.
	AmbientScene ambientScene;
	if (isActive())
	{
		_view->ambientScene(ambientScene);
	}
	_ambience->update(ambientScene.ready ? &ambientScene : nullptr, elapsedSeconds);

	// Explosions in 3D at the isometric map's pace (the battle's timer catching up).
	catchUp();
	// The numbers rising over the units hit (setting "Damage numbers"), in either view.
	updateDamageNumbers(now);

	// With "Voxel units" every unit's body is made from the battle's start (or from when the setting is switched on),
	// seen or not: none is left to make when the 3D view opens, and with "Realistic hitboxes" a shot hits the body.
	if (Options::fpsVoxelUnits)
	{
		makeVoxelBodies(VoxelBodiesMs);
	}

	// The right button may have been let go while another screen had the mouse, or the view
	// changed: then the zoom it held and the drag look end too.
	if ((_zoomButtonHeld || _dragLook) && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_RIGHT)))
	{
		_zoomButtonHeld = false;
		_dragLook = false;
	}

	// When the mouse gets captured or released, or we come back from another screen, the cursor
	// jumps; ignore mouse movement for a moment so the jump doesn't turn the view. Coming back
	// is seen by the game having released the mouse since the last frame, however briefly: a
	// window that opens and closes at once (the action menu of an item with nothing to do, a
	// lamp fixed in the hand) came back too quickly to notice otherwise, and its jump turned
	// the soldier.
	const unsigned mouseReleases = _game->getMouseReleases();
	if (wantsMouseCapture() != _mouseWasCaptured || resumed || mouseReleases != _mouseReleasesSeen)
	{
		_mouseWasCaptured = wantsMouseCapture();
		_mouseReleasesSeen = mouseReleases;
		_ignoreMotionUntil = now + 150;
	}

	// Doom IDDQD keeps X-COM's morale full, whatever the view and whoever's turn it is; Doom
	// IDCLIP switched off lets those it left in the air fall.
	applyGodMode();
	applyNoclipEnd();

	// A soldier going down or breaking down: their camera has the frame, unzoomed (the scope
	// goes).
	if (deathCam(now, elapsedSeconds) || breakdownCam(now, elapsedSeconds))
	{
		if (_zoom != 1.0)
		{
			_zoom = 1.0;
			_view->setFieldOfView(fieldOfViewNow(), _zoom);
			updateScope();
		}
		return;
	}

	// Whose eyes to look through: the selected soldier on your turn, your last soldier on the
	// aliens' turn (if that's switched on), nobody if they're out.
	const bool playerTurn = _save->getSide() == FACTION_PLAYER;
	BattleUnit *selected = selectNextSoldierIfGone(playerTurn);
	updateLastPlayerUnit(playerTurn, selected);
	// (The aliens' turn cameras may pick another of ours, see alienTurnCamera; they start afresh
	// each aliens' turn.)
	BattleUnit *alienTurnView = nullptr;
	if (playerTurn)
	{
		// The player's turn: the view starts afresh on the soldier, looking the way they face
		// (the cameras turned it round for the aliens, and turning by the rules mustn't charge
		// the soldier for that), and forgets an alien it was to turn to.
		if (_alienTurnCamerasRan)
		{
			_trackedUnit = nullptr;
			_lookAtOnSwitch = nullptr;
		}
		_alienTurnCamerasRan = false;
		_alienTurnCamera = nullptr;
		_alertWatch.clear();
	}
	else if (alienTurn3D() && _wants3D)
	{
		// (Only while the 3D view is wanted; with the map shown, see below.)
		alienTurnView = alienTurnCamera(now);
		_alienTurnCamerasRan = Options::fpsAlienTurnCameras;
	}
	else if (Options::fpsAlienTurnCameras)
	{
		// The map shows the aliens' turn: the cameras pop up over it.
		alienTurnMapCameras(now);
	}
	BattleUnit *unit = playerTurn ? selected : (alienTurn3D() ? (alienTurnView ? alienTurnView : _lastPlayerUnit) : nullptr);
	if (unit && unit->isOut())
	{
		unit = nullptr;
	}

	// Show the 3D view when the player wants it and there's someone to look through.
	const bool show3D = _wants3D && unit;
	if (_isoFlight.on)
	{
		// (3DIso's flight: 3D comes at its end, see drawIsoFlight. Wanted the other way meanwhile: it turns back from where it
		// is, the same way it came, in the time it has flown; the eased path is symmetric, so the picture doesn't jump)
		if (_isoFlight.to3D != show3D && (show3D ? unit == _isoFlight.unit : true))
		{
			const Uint32 ticks = SDL_GetTicks(), length = (Uint32)(isoFlightSeconds() * 1000.0);
			const Uint32 flown = std::min(length, ticks - _isoFlight.start);
			_isoFlight.to3D = show3D;
			_isoFlight.start = ticks - (length - flown);
		}
		else if (_isoFlight.to3D && !show3D) endIsoFlight();
	}
	else if (show3D != isActive())
	{
		if (iso3DOn() && show3D && !feedWanted() && Options::fpsIsoFlight)
		{
			startIsoFlight(unit, true);
		}
		else if (iso3DOn() && !show3D && isActive() && unit && !feedWanted() && Options::fpsIsoFlight)
		{
			BattleUnit *flown = unit;
			double eye[3];
			_view->getEye(eye);
			const double yaw = _view->getYawDegrees(), pitch = _view->getPitchDegrees();
			setActive(false); // (the map's camera goes to the soldier)
			startIsoFlight(flown, false);
			std::copy(eye, eye + 3, _isoFlight.eye);
			_isoFlight.yaw = yaw;
			_isoFlight.pitch = pitch;
		}
		else
		{
			// (3DIso on without a flight, setting "3DIso flight" Off: the flares burning and the marks go across all the same)
			if (iso3DOn() && !feedWanted())
			{
				if (show3D) _view->takeFlares(*_isoView);
				else _isoView->takeFlares(*_view);
			}
			setActive(show3D);
		}
	}
	// The 3D view's first frame drawn ahead while the map shows (setting "Renderer": GPU): its waiting is done where
	// the battle begins, not at the first click of the 3D button.
	primeView(playerTurn ? selected : _lastPlayerUnit);

	// What runs in both views: the top bar, the soldier card, the map camera, the music, aiming
	// on the isometric map, akimbo's second shot in turn and flight.
	updateTopBar(selected);
	updateSoldierCard();
	updateMapCamera(); // (after the card: a squad button's soldier cam wins)
	placeGameLine(); // (after the map camera: under its window while it shows)
	updateFeed(); // (the Live command feed, in the map's place with its setting on)
	updateIsoView(); // (3DIso, research)
	watchMapLayers();
	updateMusic(now);
	updateMapAim();
	fireTurnShot(); // (akimbo in turn: the other gun's shot, or calling it off on the map)
	updateFlight(show3D && !_isoFlight.on ? unit : nullptr);

	// On the isometric map: hide the 3D view's overlays (the scope too: back in 3D the view
	// starts unzoomed), and that's all. (So too during 3DIso's flight into the eyes: the 3D view
	// isn't there yet, and nothing may walk or turn the soldier unseen.)
	if (!show3D || _isoFlight.on)
	{
		_zoom = 1.0;
		updateScope();
		updateInfoPanel(nullptr);
		updateArmorButtons(nullptr);
		updateLift(nullptr, now);
		updateCompass(nullptr, false);
		updateIsoCompass();
		updateWalkModeLines(nullptr);
		updateCrosshair(nullptr, playerTurn);
		updateUnitName(nullptr);
		updateBanner(playerTurn);
		updateScanner(nullptr);
		updateWeaponInHand(nullptr);
		endShotAim(); // (a shot fired in 3D and still flying when the map came up: its aim ends here)
		syncShadows();
		return;
	}

	// Doom mode (debug) tops the soldier up, on your turn, and its grenades burn in real time.
	if (playerTurn && doomModeOn() && unit->getFaction() == FACTION_PLAYER)
	{
		applyDoomMode(unit);
	}
	updateDoomFuses(now);

	// Turning with the keys, and the view's settings (they can change in the options).
	turnWithKeys(elapsedSeconds);
	updateZoom(elapsedSeconds);
	// Which way the units face, while the key for it is held (Left Alt by default: it's the game's
	// modifier, so held on its own it counts whatever else is held).
	_view->setShowFacing(isKeyHeld(Options::keyFpsShowFacing));
	updateScope();
	_view->setFieldOfView(fieldOfViewNow(), _zoom);
	_view->setSmoothLighting(Options::fpsSmoothLighting);
	_view->setMaxDistance(FirstPerson::viewDistance(_game->getMod()));

	// What happened to the soldier and around them: a new enemy spotted, hurt, on fire, a shot
	// whizzing past; the recoil easing back, the game done aiming our last shot, and a held
	// trigger firing on.
	checkSpotted(unit, playerTurn);
	checkHurt(unit, now);
	checkBurning(unit, now);
	checkNearMiss();
	checkGlassBreak();
	updateRecoil(unit, elapsedSeconds);
	endShotAim();
	keepFiringWhileHeld(unit, playerTurn);

	// Night vision, in the isometric map's colour.
	_view->setNightVision(unit ? _state->_map->getNightVisionColor() : 0);
	// Smoke as this soldier's eyes see through it: the game's sight range, and the armor's heat
	// vision (the see-through smoke thinner; enemies they don't see in smoke not drawn).
	_view->setSmokeSight(_save->getTileEngine()->getMaxViewDistance() * 16.0f, unit->getVisibilityThroughSmoke(), unit->getVisibilityThroughFire());
	// The flashlight, on and off with its own key (see toggleFlashlight): a spot on the crosshair,
	// so it lights dark places by day too.
	_view->setFlashlight(_flashlightOn, true);

	// The camera follows the soldier (and walks them on your turn); on your turn the soldier
	// also faces where you look, and the fire mode and aim are kept up to date.
	updateCamera(unit, playerTurn && !feedIsoHasKeys(), elapsedSeconds); // (no walking from the 3D window while the feed's ISO has the keys)
	updateNoclipView(unit);
	if (!playerTurn)
	{
		holdViewToFacing(unit);
	}
	if (playerTurn)
	{
		openDoorAfterTurn(unit);
		if (feedIsoHasKeys()) viewFollowsFacing(unit); // (the game's turns lead: a right click on the map, say)
		else if (vrOn()) vrFaceBody(unit, elapsedSeconds);
		else faceLookDirection(unit);
		updateFireMode(unit);
		updateAim(unit);
	}
	updateTrajectory(unit, playerTurn);
	updateSprayMarks(playerTurn ? unit : nullptr);

	// Everything drawn over the view, then the view itself is redrawn.
	updateEnemyMarkers();
	updateInfoPanel(unit);
	updateArmorButtons(unit);
	updateLift(unit, now);
	updateCompass(unit, playerTurn);
	updateWalkModeLines(unit);
	updateCrosshair(unit, playerTurn);
	updateUnitName(unit);
	updateBanner(playerTurn);
	updateScanner(unit);
	updateWeaponInHand(unit);
	syncShadows();
	_view->invalidate();
}

/**
 * The soldier the camera was with is gone (killed, stunned, mind controlled...): carries on
 * with the next one, as soon as the game is free to (not busy, no popup up). Only on the
 * player's turn and while the player wants the 3D view. With "Quick deaths" the view went on
 * to the next soldier at once (see quickHandOver): until the game is free (a burst or reaction
 * fire still under way) the view looks through them already, and then they're the one selected.
 * @return The selected unit (now), or the soldier the view went on to.
 */
BattleUnit *FirstPersonMode::selectNextSoldierIfGone(bool playerTurn)
{
	BattleUnit *selected = _save->getSelectedUnit();
	const bool gone = !selected || selected->isOut() || selected->getFaction() != FACTION_PLAYER;
	if (!gone || !playerTurn || (_quickNextSoldier && (_quickNextSoldier->isOut() || _quickNextSoldier->getFaction() != FACTION_PLAYER)))
	{
		_quickNextSoldier = nullptr; // (not needed, or no longer standing)
	}
	// The soldier we were with is gone (killed, stunned, mind controlled...): carry on with the next one.
	if (_wants3D && playerTurn && gone && !_state->_battleGame->isBusy() && _state->_popups.empty())
	{
		if (_quickNextSoldier)
		{
			// (kept till the game really takes them: it may not take orders yet)
			selectUnit(_quickNextSoldier);
			if (_save->getSelectedUnit() == _quickNextSoldier)
			{
				_quickNextSoldier = nullptr;
			}
		}
		else
		{
			_state->selectNextPlayerUnit();
		}
		selected = _save->getSelectedUnit();
	}
	return _quickNextSoldier ? _quickNextSoldier : selected;
}

/**
 * Keeps track of the last soldier the player had selected: the view stays with them during
 * the alien turn (if that's switched on), or with another soldier still standing when they
 * are gone.
 */
void FirstPersonMode::updateLastPlayerUnit(bool playerTurn, BattleUnit *selected)
{
	// On the player's turn, the selected soldier (if one of ours and standing).
	if (playerTurn && selected && selected->getFaction() == FACTION_PLAYER && !selected->isOut())
	{
		_lastPlayerUnit = selected;
	}

	// That soldier went down or was taken over: the first of ours still standing instead (or
	// nobody).
	if (_lastPlayerUnit && (_lastPlayerUnit->isOut() || _lastPlayerUnit->getFaction() != FACTION_PLAYER))
	{
		_lastPlayerUnit = nullptr;
		for (BattleUnit *unit : *_save->getUnits())
		{
			if (unit->getFaction() == FACTION_PLAYER && !unit->isOut())
			{
				_lastPlayerUnit = unit;
				break;
			}
		}
	}
}

/**
 * Gets the first look at every event, before the battlescape screen's own handling.
 * Keys: the view toggle anywhere; in 3D the mouse mode, classic HUD and doom toggles, the fire
 * mode and hand keys, up/down, the enemy keys (they turn the view to that enemy), and the
 * movement keys (polled each frame, swallowed here so they don't fire battlescape hotkeys);
 * everything else goes on to the normal hotkeys.
 * Mouse: in mouse look it turns the view, the left button fires, the right button cancels a
 * picked fire mode, the wheel changes the fire mode; with a free cursor our buttons get it
 * first, then the classic HUD (if shown), and a left click on the view fires at the cursor.
 * @return True if the event was consumed.
 */
bool FirstPersonMode::handle(Action *action)
{
	// (an event the Live command feed hands the battle without the map: the battle's, see feedWithoutMap)
	if (_feedDispatching)
	{
		return false;
	}
	const SDL_Event &event = *action->getDetails();
	const bool active = isActive();

	// 3DIso's flight (research): a moment between the map and the soldier's eyes, which the battle waits out. Only the 3D key
	// and button (they turn it back) and 3DIso's own switch work; no other key, click or wheel reaches the map or the battle.
	if (_isoFlight.on)
	{
		if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
		{
			const SDL_Keycode key = event.key.keysym.sym;
			const bool flightKey = key == Options::keyBattleFirstPerson || (key == Options::keyBattleFirstPerson2 && key != SDLK_UNKNOWN) || key == Options::keyFpsIsoToggle;
			return flightKey ? handleKey(action, active) : true;
		}
		if (isMouseButtonOrWheel(event))
		{
			int eventX = 0, eventY = 0;
			mouseEventPosition(event, eventX, eventY);
			action->setMouseAction(eventX, eventY, 0, 0);
			if (buttonAt(toCanvasX(action->getAbsoluteXMouse()), toCanvasY(action->getAbsoluteYMouse())) == _btnView) return handleFreeCursor(action, active);
			return true;
		}
	}

	// Which squad button (for the soldier card) and lift arrow the free cursor is over (never consumed).
	if (event.type == SDL_MOUSEMOTION && !wantsMouseCapture())
	{
		trackHover(action);
	}

	// 3DIso's free camera (research): the mouse's drags and wheel over the map are its (see handleIsoFreeCamera).
	if (!active && _isoFree && iso3DOn() && _isoView->getVisible() && !_isoFlight.on && handleIsoFreeCamera(action))
	{
		return true;
	}
	// 3DIso: Ctrl and the wheel zoom at the mouse (the point under it stays where it is: the map's offset moves by the change, the
	// picture gliding with it, see updateIsoView).
	if (!active && wheelStep(event) != 0 && (SDL_GetModState() & KMOD_CTRL) && iso3DOn() && _isoView->getVisible())
	{
		int eventX = 0, eventY = 0;
		mouseEventPosition(event, eventX, eventY);
		action->setMouseAction(eventX, eventY, 0, 0);
		isoZoomAt(action->getAbsoluteXMouse(), action->getAbsoluteYMouse(), wheelStep(event) < 0 ? 1.25 : 0.8); // (up: in)
		return true;
	}

	// The mouse's thumb buttons are the game's (next and previous soldier, OXCE's option).
	if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) && (event.button.button == SDL_BUTTON_X1 || event.button.button == SDL_BUTTON_X2))
	{
		return false;
	}

	// Keys go to the key handler; anything that's neither a key nor the mouse (its moves, buttons
	// and wheel) isn't ours.
	if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
	{
		if (feedIsoHasKeys())
		{
			// (the feed's ISO: our keys that work on the isometric map, then the battle's as there, see feedIsoKey)
			return handleKey(action, false) || feedIsoKey(action);
		}
		return handleKey(action, active);
	}
	if (event.type != SDL_MOUSEMOTION && !isMouseButtonOrWheel(event))
	{
		return false;
	}

	// The mouse: captured for looking around in 3D, or a free cursor.
	if (active && wantsMouseCapture())
	{
		return handleMouseLook(action);
	}
	return handleFreeCursor(action, active);
}

/**
 * Notes which squad button (for the soldier card) and which lift arrow (to light it up) the
 * free cursor is over, on every mouse movement.
 */
void FirstPersonMode::trackHover(Action *action)
{
	// Where the mouse is, on the canvas our buttons and arrows live on (and the view: kept, for
	// what the free cursor points at).
	const SDL_Event &event = *action->getDetails();
	action->setMouseAction(event.motion.x, event.motion.y, 0, 0);
	_cursorX = (int)(toCanvasX(action->getAbsoluteXMouse()));
	_cursorY = (int)(toCanvasY(action->getAbsoluteYMouse()));
	const TextButton *hoveredButton = buttonAt(toCanvasX(action->getAbsoluteXMouse()), toCanvasY(action->getAbsoluteYMouse()));
	_liftArrowHovered = liftArrowAt(toCanvasX(action->getAbsoluteXMouse()), toCanvasY(action->getAbsoluteYMouse()));

	// Which squad button that is, if any (-1: none). Only when the cursor comes onto another
	// button: the wheel may have moved the card on from the one under it (see browseSquad).
	if (hoveredButton != _buttonUnderCursor)
	{
		_buttonUnderCursor = hoveredButton;
		_hoveredSquadButton = -1;
		for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
		{
			if (hoveredButton == _btnSquad[buttonIndex]) _hoveredSquadButton = (int)buttonIndex;
		}
	}
}

/**
 * A key went down or up: the view toggle anywhere; in 3D our own keys. Each of our keys acts
 * when it goes down, and both its down and up are ours. The movement keys are polled each
 * frame, and swallowed here so they don't fire battlescape hotkeys. With the game's Ctrl or Alt
 * held the key belongs to the game (not with a Ctrl or Alt that is the lean key, held to lean).
 * @param active Whether the 3D view is showing.
 * @return True if the key was ours.
 */
bool FirstPersonMode::handleKey(Action *action, bool active)
{
	const SDL_Event &event = *action->getDetails();
	const SDL_Keycode key = event.key.keysym.sym;
	// A key SDL can't name is never ours (an unbound action is SDLK_UNKNOWN too), nor one with
	// the game's Ctrl or Alt held (a shortcut; see gameModifierHeld).
	if (key == SDLK_UNKNOWN || gameModifierHeld())
	{
		return false;
	}
	// Went down (not up). Caps Lock and Num Lock are different in SDL 1.2: it sends a lock key's
	// "down" when its lock goes on and its "up" when it goes off, one or the other for each
	// press, so for them either is a press (SDL2 sends both, as for any key).
	const bool lockKey = LockKeysSendOneEvent && (key == SDLK_CAPSLOCK || key == NumLockKey);
	const bool pressed = event.type == SDL_KEYDOWN || (lockKey && event.type == SDL_KEYUP);
	// (While leaning with Left Shift, the game's own hotkeys stay quiet, as the game ignores a
	// hotkey with Shift held: Q and E don't open a hand's menu in the middle of a lean.)

	// The 3D key works in both views; on the isometric map, the 3D HUD key shows or hides the
	// map camera.
	if (key == Options::keyBattleFirstPerson || (key == Options::keyBattleFirstPerson2 && key != SDLK_UNKNOWN))
	{
		if (pressed) toggleView();
		return true;
	}
	if (!active && key == Options::keyFpsHideHud && !feedShown()) // (the Live command feed shows no map camera: the key leaves its setting alone)
	{
		if (pressed) toggleMapCamera();
		return true;
	}
	if (handleFeedKey(action))
	{
		return true;
	}
	// The grab key (no key until the player gives it one) works in both views too.
	if (key != SDLK_UNKNOWN && key == Options::keyFpsGrabItem)
	{
		if (pressed) grabItemUnderfoot(action);
		return true;
	}
	// 3DIso (research): its switches act on the press itself, so a frame held up by something else (a save, OXCE's voxel
	// pictures) can't lose one, and while 3DIso shows the key is its alone (see updateIsoView).
	// (A key left unbound, as most of them are, is SDLK_UNKNOWN: never one the system can't name.)
	if (!active && key != SDLK_UNKNOWN && (getenv("OXC3D_ISO3D") || Options::fpsIso) && _isoView)
	{
		const bool shown = _isoMode == 1 && !_isoFlight.on;
		if (key == Options::keyFpsIsoToggle) { if (pressed) _isoMode = 1 - _isoMode; return true; }
		if (shown && key == Options::keyFpsIsoArt) { if (pressed) toggleIsoArt(); return true; }
		if (shown && key == Options::keyFpsIsoEffects) { if (pressed) _isoExtrasOn = !_isoExtrasOn; return true; }
		if (shown && key == Options::keyFpsIsoReset) { if (pressed) _isoResetAsked = true; return true; }
		if (shown && key == Options::keyFpsIsoFree) { if (pressed) setIsoFree(!_isoFree); return true; }
	}
	if (!active)
	{
		return false;
	}

	// Our keys in 3D: the modes, fire modes and hand, doors, flares, up/down, flight, the
	// flashlight, sprinting and en passant, jump and night vision. (The flare and flashlight keys
	// with their settings off say so on the warning line.)
	if (key == Options::keyFpsMouseMode) { if (pressed) setMouseLook(!_mouseLook); return true; }
#if SDL_VERSION_ATLEAST(2, 0, 0)
	// Android's Back key (the Meta Quest controllers' B and Y) does the same: a controller has no
	// middle button or keyboard to free the cursor with, for the top bar's buttons.
	if (key == SDLK_AC_BACK) { if (pressed) setMouseLook(!_mouseLook); return true; }
#endif
	if (key == Options::keyFpsClassicHud) { if (pressed) setClassicHud(!_classicHud); return true; }
	if (key == Options::keyFpsHideHud) { if (pressed) toggleHud(); return true; }
	if (key == Options::keyFpsDoomMode) { if (pressed) toggleCheats(); return true; }
	if (key == Options::keyFpsNextFireMode) { if (pressed) cycleFireMode(1); return true; }
	if (key == Options::keyFpsModeSnap) { if (pressed) selectFireMode(BA_SNAPSHOT); return true; }
	if (key == Options::keyFpsModeAimed) { if (pressed) selectFireMode(BA_AIMEDSHOT); return true; }
	if (key == Options::keyFpsModeAuto) { if (pressed) selectFireMode(BA_AUTOSHOT); return true; }
	if (key == Options::keyFpsSwitchHand) { if (pressed) switchHand(); return true; }
	// (the turn keys: in VR mode the right stick's turn, a snap turn of the soldier and the world if snap turning, else held
	// for a smooth turn; see vrSnapTurn, vrFaceBody)
	if (key == Options::keyFpsTurnLeft) { if (pressed) { if (vrOn()) vrTurnKey(-1); else { _snapTurnDegrees -= 45.0; _snapTurnFacing = true; } } return true; }
	if (key == Options::keyFpsTurnRight) { if (pressed) { if (vrOn()) vrTurnKey(1); else { _snapTurnDegrees += 45.0; _snapTurnFacing = true; } } return true; }
	if (key == Options::keyFpsAkimbo) { if (pressed) toggleAkimbo(); return true; }
	if (key != SDLK_UNKNOWN && key == Options::keyFpsAkimboStyle) { if (pressed) toggleAkimboStyle(); return true; }
	// The lean keys (setting "Lean") are read each frame while held (see updateLean); here they
	// are only swallowed. With leaning off they're the game's.
	if (Options::fpsLean > 0 && key != SDLK_UNKNOWN && (key == Options::keyFpsLeanHold || key == Options::keyFpsLeanLeftOwn || key == Options::keyFpsLeanRightOwn)) return true;
	if (key != SDLK_UNKNOWN && key == Options::keyFpsZoom) return true; // (held: see updateZoom)
	if (key != SDLK_UNKNOWN && key == Options::keyFpsShowFacing) return true; // (held: see think)
	if (key != SDLK_UNKNOWN && key == Options::keyFpsRange) return true; // (held: see updateCrosshair)
	if (key == Options::keyFpsOpenDoor) { if (pressed) openDoor(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsFlare) { if (pressed) throwFlare(); return true; }
	if (key == Options::keyFpsUp) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_UP, action, true); return true; }
	if (key == Options::keyFpsDown) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_DOWN, action, true); return true; }
	if (key == Options::keyFpsFlight) { if (pressed) toggleFlight(_save->getSelectedUnit()); return true; }
	if (key != SDLK_UNKNOWN && key == Options::keyFpsTurret) { if (pressed) toggleTurret(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsFlashlight) { if (pressed) toggleFlashlight(); return true; }
	if (key == Options::keyFpsSprint) { if (pressed) toggleSprint(); return true; }
	if (key == Options::keyFpsEnPassant) { if (pressed) toggleEnPassant(); return true; }
	if (key == Options::keyFpsJump) { if (pressed && _zoom <= 1.05) jump(_save->getSelectedUnit()); return true; } // (zoomed in, it looks closer still: see updateZoom)
	if (key != SDLK_UNKNOWN && key == Options::keyFpsEmergencyClimb) { if (pressed) emergencyClimb(_save->getSelectedUnit()); return true; }
	// Night vision: the gear of the soldier the view looks through (in the aliens' turn the
	// game's selected unit is the alien acting); the game's own key (Scroll Lock) checks for it
	// in 3D too.
	if (key == Options::keyFpsNightVision || key == Options::keyNightVisionToggle) { if (pressed) toggleNightVision(); return true; }

	// The game's reload key (R): the game's own reload, and the warning line says why when it loads
	// nothing (see reload). With Ctrl, Alt or Shift held it stays the game's, which then ignores it,
	// as it does every hotkey (see InteractiveSurface::keyboardPress).
	if (key == Options::keyBattleReload && !(event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)))
	{
		if (pressed) reload(action);
		return true;
	}

	// The game's own keys for what acts on the isometric map, translated (see classicHudButton):
	// the centre key (Home) looks straight ahead; the map level keys (if not taken by up/down
	// above) look a level up or down; the soldier up/down keys move as ours do; and the minimap
	// key (M) opens the minimap on the soldier (the map's camera goes there first, then the
	// game opens it).
	if (key != SDLK_UNKNOWN)
	{
		const BattlescapeState *s = _state;
		if (key == Options::keyBattleCenterUnit) { if (pressed) classicHudButton(s->_btnCenter, action); return true; }
		if (key == Options::keyBattleLevelUp) { if (pressed) classicHudButton(s->_btnMapUp, action); return true; }
		if (key == Options::keyBattleLevelDown) { if (pressed) classicHudButton(s->_btnMapDown, action); return true; }
		if (key == Options::keyBattleUnitUp) { if (pressed) classicHudButton(s->_btnUnitUp, action); return true; }
		if (key == Options::keyBattleUnitDown) { if (pressed) classicHudButton(s->_btnUnitDown, action); return true; }
		if (key == Options::keyBattleMap && pressed && _save->getSelectedUnit())
		{
			_state->_map->getCamera()->centerOnPosition(_save->getSelectedUnit()->getPosition());
			return false;
		}
	}

	// The numpad (after the bindable keys, which may use it): moves a tile at a time, almost at
	// once, as on the isometric map, whatever the movement setting: 8 ahead, 2 back, 4 and 6
	// sideways, relative to the view, two of them pressed together a diagonal (a player: as W, A,
	// S and D do); 7 and 9 a snap turn. A press is remembered until the move can start (see
	// tileMoveVelocity), so a quick tap always counts; a key of the other axis pressed within
	// NumpadComboWait of it joins it.
	if (key == KeypadKey8 || key == KeypadKey2 || key == KeypadKey4 || key == KeypadKey6)
	{
		if (pressed && _save->getSide() == FACTION_PLAYER) // (not remembered through the aliens' turn)
		{
			const int forward = key == KeypadKey8 ? 1 : key == KeypadKey2 ? -1 : 0, strafe = key == KeypadKey6 ? 1 : key == KeypadKey4 ? -1 : 0;
			const Uint32 now = SDL_GetTicks();
			const bool joins = (_numpadMove[0] || _numpadMove[1]) && now - _numpadPressAt <= NumpadComboWait;
			if (joins)
			{
				if (forward) _numpadMove[0] = forward;
				if (strafe) _numpadMove[1] = strafe;
			}
			else
			{
				_numpadMove[0] = forward;
				_numpadMove[1] = strafe;
				_numpadPressAt = now;
			}
		}
		return true;
	}
	if (key == KeypadKey7 || key == KeypadKey9)
	{
		if (pressed && vrOn())
		{
			vrSnapTurn(key == KeypadKey7 ? -1 : 1); // (VR mode: a snap turn of the soldier and the world, see vrSnapTurn)
		}
		else if (pressed)
		{
			_snapTurnDegrees += key == KeypadKey7 ? -45.0 : 45.0;
			_snapTurnFacing = true;
		}
		return true;
	}

	// The game's "centre on enemy" keys (1 to 10) turn the view to that enemy instead (an
	// unbound key, SDLK_UNKNOWN, matches nothing).
	const SDL_Keycode enemyKeys[] = { Options::keyBattleCenterEnemy1, Options::keyBattleCenterEnemy2, Options::keyBattleCenterEnemy3,
		Options::keyBattleCenterEnemy4, Options::keyBattleCenterEnemy5, Options::keyBattleCenterEnemy6, Options::keyBattleCenterEnemy7,
		Options::keyBattleCenterEnemy8, Options::keyBattleCenterEnemy9, Options::keyBattleCenterEnemy10 };
	for (int enemyIndex = 0; enemyIndex < 10; ++enemyIndex)
	{
		if (key == enemyKeys[enemyIndex] && key != SDLK_UNKNOWN)
		{
			if (pressed) lookAtEnemy(enemyIndex);
			return true;
		}
	}

	// The movement keys are read each frame (see updateMovement); here they are only swallowed.
	return key == Options::keyFpsForward || key == Options::keyFpsBack
		|| key == Options::keyFpsStrafeLeft || key == Options::keyFpsStrafeRight;
}

/**
 * The mouse in mouse look: moving turns the view, the left button fires (held: the trigger
 * stays pulled), the right button cancels a picked action (or ends a drag look), the middle
 * button frees the cursor and the wheel changes the fire mode. All of it is ours.
 * @return Always true (consumed).
 */
bool FirstPersonMode::handleMouseLook(Action *action)
{
	const SDL_Event &event = *action->getDetails();
	const bool wheelTurned = wheelStep(event) != 0;

	if (event.type == SDL_MOUSEMOTION)
	{
		// Turn the view, except right after the mouse was captured (the cursor's jump to the
		// window centre isn't a look). A screen that was on top only for a moment can hand the
		// battlescape that jump before think() runs again, so a release is looked for here too.
		if (_game->getMouseReleases() != _mouseReleasesSeen)
		{
			_mouseReleasesSeen = _game->getMouseReleases();
			_ignoreMotionUntil = SDL_GetTicks() + 150;
		}
		if (SDL_GetTicks() >= _ignoreMotionUntil)
		{
			turnByMouse(event.motion.xrel, event.motion.yrel);
		}
	}
	else if (wheelTurned)
	{
		// The wheel steps through the fire modes (up: the previous one).
		cycleFireMode(wheelStep(event));
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_MIDDLE && !_dragLook)
	{
		// The middle button shows the unit under the crosshair (setting "Middle click
		// identifies"), or else frees the cursor.
		if (!identifyPointedUnit()) setMouseLook(false);
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT && !_dragLook)
	{
		// The left button pulls the trigger: a shot at the middle of the view (the crosshair).
		_triggerHeld = true;
		_burstConfirmed = false;
		fire(viewPixelX(canvasWidth() / 2), viewPixelY(canvasHeight() / 2));
	}
	else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
	{
		// ...and letting go releases it (in FPS-style auto, no further burst follows).
		_triggerHeld = false;
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT && !_dragLook)
	{
		// The right button forgets a target marked for confirm fire, or cancels a picked action
		// (or a walk), as on the isometric map; with nothing to cancel, it opens the closed door
		// the crosshair points at, when that is within the soldier's reach, as a right click on a
		// door does there (setting "Right click opens doors"; a player, GitHub issue #244: "greater
		// iso parity ... if you are directly on the door"); and with no such door, it zooms while
		// held.
		if (_confirmTile.x >= 0) clearConfirmMark();
		else if (!_state->_battleGame->cancelCurrentAction())
		{
			BattleUnit *unit = _save->getSelectedUnit();
			const bool atDoor = Options::fpsRightClickDoors && unit && !unit->isBigUnit() && canAct() && _state->playableUnitSelected()
				&& unit->getStatus() == STATUS_STANDING && !_state->_battleGame->isBusy() && doorWithinReach(unit, true) >= 0;
			if (atDoor) openDoor(unit);
			else _zoomButtonHeld = true;
		}
	}
	else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_RIGHT)
	{
		// Letting go of the right button stops the zoom, or ends a drag look (back to the free
		// cursor).
		_zoomButtonHeld = false;
		_dragLook = false;
	}
	return true;
}

/**
 * The mouse with a free cursor: our buttons get it first, then the lift arrows and the visible
 * enemy buttons; over the view a left click fires at the cursor, the right button drags the
 * view round and the middle button goes back to mouse look. The wheel over a soldier's button
 * moves the soldier card (and the soldier cam) on through the squad, over previous / next it
 * scrolls the squad buttons, and over the view it changes the fire mode.
 * @param active Whether the 3D view is showing (on the isometric map only our buttons count).
 * @return True if the event was consumed.
 */
bool FirstPersonMode::handleFreeCursor(Action *action, bool active)
{
	const SDL_Event &event = *action->getDetails();
	const bool wheelTurned = wheelStep(event) != 0;

	// Moving over the map camera on the isometric map: the map's cursor stays where it was (and
	// an aimed action's target with it, which the camera looks at), not under the picture.
	if (!active && event.type == SDL_MOUSEMOTION && onMapCamera(_cursorX, _cursorY))
	{
		return true;
	}
	// The Live command feed's: the mouse's moves (its buttons and wheel below, after our top bar's buttons).
	if (event.type == SDL_MOUSEMOTION && feedShown())
	{
		return handleFeedMouse(action);
	}

	if (isMouseButtonOrWheel(event))
	{
		// Where the click (or the wheel's notch) is, in the game's screen pixels, and which of our
		// buttons is there.
		int eventX = 0, eventY = 0;
		mouseEventPosition(event, eventX, eventY);
		action->setMouseAction(eventX, eventY, 0, 0);
		const double mouseX = action->getAbsoluteXMouse(), mouseY = action->getAbsoluteYMouse();
		TextButton *buttonUnderCursor = buttonAt(toCanvasX(mouseX), toCanvasY(mouseY)); // our buttons are on the canvas
		if (buttonUnderCursor && feedShown() && !feedOverlay(buttonUnderCursor))
		{
			buttonUnderCursor = nullptr; // (one the Live command feed doesn't show, under its windows: not clicked)
		}

		// Let every button see the release, so none stays drawn pressed.
		if (event.type == SDL_MOUSEBUTTONUP)
		{
			for (TextButton *button : _btnSquad) handleOnCanvas(button, action);
			for (TextButton *button : { _btnView, _btnHud, _btnInventory, _btnIsoArt, _btnIsoFree, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnTurret, _btnPrev, _btnNext })
			{
				handleOnCanvas(button, action);
			}
		}

		// The wheel over a soldier's button moves their card (and the soldier cam, if open) on to
		// the next or previous soldier (up: the previous one), to browse the squad by their helmet
		// cameras without moving the mouse. Over previous / next it scrolls the squad buttons, and
		// so does a right click on them, which only scrolls (no soldier is selected).
		const bool overSoldier = buttonUnderCursor && std::find(_btnSquad.begin(), _btnSquad.end(), buttonUnderCursor) != _btnSquad.end();
		if (wheelTurned && overSoldier)
		{
			browseSquad(wheelStep(event), toCanvasX(mouseX), toCanvasY(mouseY));
			return true;
		}
		if (wheelTurned && (buttonUnderCursor == _btnPrev || buttonUnderCursor == _btnNext))
		{
			scrollSquad(wheelStep(event));
			return true;
		}
		if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT && (buttonUnderCursor == _btnPrev || buttonUnderCursor == _btnNext))
		{
			scrollSquad(buttonUnderCursor == _btnPrev ? -1 : 1);
			return true;
		}

		// Any click on one of our buttons is ours; a left press clicks it. A right click on a
		// soldier's button opens the soldier cam (their card's camera, enlarged), or closes it.
		if (buttonUnderCursor)
		{
			if (event.type == SDL_MOUSEBUTTONDOWN)
			{
				handleOnCanvas(buttonUnderCursor, action); // pressed look and click sound
				if (event.button.button == SDL_BUTTON_LEFT)
				{
					buttonClicked(buttonUnderCursor, action);
				}
				else if (event.button.button == SDL_BUTTON_RIGHT && std::find(_btnSquad.begin(), _btnSquad.end(), buttonUnderCursor) != _btnSquad.end())
				{
					_soldierCamOpen = !_soldierCamOpen;
				}
			}
			return true;
		}

		// The Live command feed's windows, and the map in its drone's window (see handleFeedMouse).
		if (feedShown())
		{
			return handleFeedMouse(action);
		}

		// On the isometric map, the map camera: a left click on it goes to the 3D view (the
		// selected soldier's), a right click opens the soldier cam in its place, or closes it. The
		// map under it gets nothing: no soldier walks off by a click on the picture.
		if (!active && onMapCamera((int)(toCanvasX(mouseX)), (int)(toCanvasY(mouseY))))
		{
			if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
			{
				_mapSoldierCamOpen = false;
				toggleView();
			}
			else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT)
			{
				_mapSoldierCamOpen = !_mapSoldierCamOpen;
			}
			return true;
		}

		// On the isometric map, the rest belongs to the map.
		if (!active)
		{
			return false;
		}

		// The lift arrows ride the lift (the up/down keys do the same).
		if (const int liftArrow = liftArrowAt(toCanvasX(mouseX), toCanvasY(mouseY))) // 1 up, -1 down
		{
			if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
			{
				moveUpDown(_save->getSelectedUnit(), liftArrow > 0 ? Pathfinding::DIR_UP : Pathfinding::DIR_DOWN, action);
			}
			return true;
		}

		// The game's own buttons, where the map's handling would move the hidden map instead: the
		// visible enemy buttons (in full 3D with our overlays, see drawEnemyButtons; with the
		// classic HUD on the screen itself, in its place), and with the classic HUD, its buttons
		// that act on the map.
		auto under = [&](const InteractiveSurface *button) {
			return button->getVisible() && mouseX >= button->getX() && mouseX < button->getX() + button->getWidth()
				&& mouseY >= button->getY() && mouseY < button->getY() + button->getHeight();
		};
		const bool press = event.type == SDL_MOUSEBUTTONDOWN && (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT);
		const bool release = event.type == SDL_MOUSEBUTTONUP && (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT);
		if (press || release)
		{
			// An enemy button: the left button turns the view to that enemy, the right one picks
			// the soldier who sees it with the most time units left, looking at it (the map would
			// centre on the enemy, or on that soldier).
			for (int enemyIndex = 0; enemyIndex < BattlescapeState::VISIBLE_MAX && (_classicHud || !hudHidden()); ++enemyIndex)
			{
				// (in full 3D where drawEnemyButtons draws them, with the classic HUD in its own place)
				if (_classicHud ? !under(_state->_btnVisibleUnit[enemyIndex]) : !onEnemyButton(enemyIndex, toCanvasX(mouseX), toCanvasY(mouseY))) continue;
				if (press && event.button.button == SDL_BUTTON_LEFT) lookAtEnemy(enemyIndex);
				if (press && event.button.button == SDL_BUTTON_RIGHT) lookFromBestSpotter(enemyIndex);
				return true;
			}
			// A classic HUD button that acts on the map: translated on the press (the release is
			// swallowed too, so the game's button doesn't click as well).
			if (_classicHud)
			{
				const InteractiveSurface *mapButtons[] = { _state->_btnUnitUp, _state->_btnUnitDown, _state->_btnMapUp, _state->_btnMapDown,
					_state->_btnShowMap, _state->_btnCenter };
				for (const InteractiveSurface *button : mapButtons)
				{
					if (!under(button)) continue;
					if (press && event.button.button == SDL_BUTTON_LEFT) classicHudButton(button, action);
					return true;
				}
			}
		}

		// A press on the view itself (not on the classic HUD, if shown): the wheel changes the
		// fire mode, the middle button goes back to mouse look, the right button starts a drag
		// look, and the left button fires at the cursor.
		const bool overClassicHud = _classicHud && mouseY >= _state->_icons->getY();
		if ((event.type == SDL_MOUSEBUTTONDOWN || wheelTurned) && !overClassicHud) // (SDL2's wheel isn't a button)
		{
			if (wheelTurned)
			{
				cycleFireMode(wheelStep(event));
				return true;
			}
			if (event.button.button == SDL_BUTTON_MIDDLE)
			{
				if (!identifyPointedUnit()) setMouseLook(true);
				return true;
			}
			if (event.button.button == SDL_BUTTON_RIGHT)
			{
				_dragLook = true;
				return true;
			}
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				fire(viewPixelX((int)(toCanvasX(mouseX))), viewPixelY((int)(toCanvasY(mouseY)))); // the canvas's pixel under the cursor, on the view
				return true;
			}
		}
	}
	if (!active)
	{
		return false;
	}
	// With the classic HUD up, the normal handling takes it from here; without it there's nothing else to click.
	return !_classicHud;
}


/**
 * Draws the whole battlescape screen, every frame and in both views, in place of the state's
 * own drawing: the 3D view at the back (when it shows), then the game's own surfaces (in full
 * 3D, without the classic HUD, only its warnings and debug text), then our overlays on top (the
 * top bar shows over the isometric map too), with them in full 3D the original's visible
 * enemy buttons. On big screens the view and the overlays are drawn on the canvas and magnified.
 * @return Always true: the screen is drawn here, so the state doesn't draw it itself.
 */

bool FirstPersonMode::blit()
{
	// (OXC3D_ISO3D_TIME, or OPENXCOM3D_GPU_TIMING: the whole frame in the log, averaged over 120 frames, for a slow game: how often
	// a frame comes, its worst, the battlescape's drawing here and the screen's flip after it, at the display's size; the rest
	// is the game's own work and its wait for the next frame)
	struct FrameLog
	{
		bool on;
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		Game *game;
		~FrameLog()
		{
			if (!on) return;
			static double everySum = 0.0, worst = 0.0, drawnSum = 0.0, flipSum = 0.0;
			static int frames = 0;
			static std::chrono::steady_clock::time_point previous;
			static bool begun = false;
			const auto now = std::chrono::steady_clock::now();
			if (begun)
			{
				const double every = std::chrono::duration<double, std::milli>(started - previous).count();
				everySum += every;
				worst = std::max(worst, every);
				drawnSum += std::chrono::duration<double, std::milli>(now - started).count();
				flipSum += FirstPerson::lastFlipTime();
				if (++frames == 120)
				{
					const SDL_Surface *display = SDL_GetVideoSurface();
					Log(LOG_INFO) << "OpenXcom3D: the frame every " << everySum / frames << " ms (" << (int)std::lround(1000.0 * frames / std::max(1.0, everySum))
						<< " fps, worst " << worst << " ms): the battlescape drawn in " << drawnSum / frames << " ms, the flip " << flipSum / frames << " ms (display "
						<< (display ? display->w : 0) << "x" << (display ? display->h : 0) << "x" << (display ? (int)display->format->BitsPerPixel : 0) << ", the game's screen "
						<< game->getScreen()->getWidth() << "x" << game->getScreen()->getHeight() << ")";
					everySum = worst = drawnSum = flipSum = 0.0;
					frames = 0;
				}
			}
			previous = started;
			begun = true;
		}
	};
	static const bool frameTime = (getenv("OXC3D_ISO3D_TIME") && *getenv("OXC3D_ISO3D_TIME")) || (getenv("OPENXCOM3D_GPU_TIMING") && *getenv("OPENXCOM3D_GPU_TIMING"));
	FrameLog frameLog{ frameTime, std::chrono::steady_clock::now(), _game };
	SDL_Surface *screen = _game->getScreen()->getSurface();
	// The Live command feed: the whole screen is its (the 3D view in its window, see blitFeed).
	if (feedShown())
	{
		blitFeed(screen);
		return true;
	}
	const bool full3D = isActive() && !_classicHud;

	// VR mode: the view looks as the head does, whatever turned it during the frame. The desktop's
	// preview draws both eyes instead, the HUD in them (and that's the whole screen); with the
	// shell drawing the eyes (see FirstPerson::vrRenderEyes), the view isn't drawn here at all.
	const bool shellDrawsEyes = vrOn() && _vrShellHead;
	if (vrOn())
	{
		vrApplyHeadAxes();
		if (Options::fpsVrMode == 2 && full3D && !_vrShellHead)
		{
			vrBlitPreview(screen);
			return true;
		}
	}

	// The 3D view at the back. The display's own size (setting "3D resolution": Display): it isn't
	// drawn here at all, but laid into the display when the engine shows the frame, wherever the
	// screen still holds the key it is filled with now (see FirstPerson::presentView). The
	// canvas's size on a big screen: magnified, straight from the view (it is in the same
	// palette: no copy onto the canvas first). The screen's own size: drawn as it is.
	// (With the shell drawing VR's eyes, neither: the view isn't shown on the screen.)
	const bool composing = _canvasOnDisplay && _composed && _canvas && _view->getVisible() && !shellDrawsEyes;
	if (composing)
	{
		// (With the canvas on the display too: the view and the overlays are put together below, and that is laid in.)
		if (_view->shown()) _view->drawIfAsked();
		SDL_FillRect(screen, nullptr, FirstPerson::presentKey(screen));
	}
	else if (_view->getVisible() && !shellDrawsEyes)
	{
		if (viewAtDisplaySize())
		{
			if (_view->shown())
			{
				_view->drawIfAsked();
				SDL_FillRect(screen, nullptr, FirstPerson::presentKey(screen));
				FirstPerson::presentView(_view);
			}
		}
		else if (_canvas && _view->getWidth() == canvasWidth())
		{
			if (_view->shown())
			{
				_view->drawIfAsked();
				magnify(screen, _view->getSurface(), false);
			}
		}
		else
		{
			_view->blit(screen);
		}
	}

	// The game's own surfaces (in full 3D only its warnings and debug text). 3DIso is drawn in the
	// map's place, under the HUD, and the map then isn't (where 3DIso is empty, the screen's black shows).
	const bool isoShown = _isoView && _isoView->getVisible() && !full3D;
	// (3DIso at the display's own resolution, extra "display": laid into the display wherever the HUD leaves the key, as the 3D
	// view at "3D resolution: Display", see FirstPerson::presentView)
	// (only at the display's picture's own size, which a frame after the display changed it may not be yet; at the map's own
	// size too when it's in full colour, which only the display can show)
	int pictureX = 0, pictureY = 0, pictureWidth = 0, pictureHeight = 0;
	if (isoShown) _game->getScreen()->getPictureArea(pictureX, pictureY, pictureWidth, pictureHeight);
	const bool isoMapSized = isoShown && _isoView->getWidth() == _state->_map->getWidth() && _isoView->getHeight() == _state->_map->getHeight();
	const bool isoPresented = isoShown && _isoView->getWidth() == pictureWidth && _isoView->getHeight() == pictureHeight
		&& (!isoMapSized || _isoView->isoFullColours());
	if (isoPresented) SDL_FillRect(screen, nullptr, FirstPerson::presentKey(screen));
	for (Surface *surface : _state->_surfaces)
	{
		if (surface == _isoView)
		{
			if (isoPresented)
			{
				_isoView->drawIfAsked();
				FirstPerson::presentView(_isoView, _isoView->isoFullColours());
			}
			else if (isoMapSized) surface->blit(screen);
			continue;
		}
		if (_isoHudSlide > 0.0 && !_ownSurfaces.count(surface) && surface != _state->_map && !full3D)
		{
			// (3DIso's flight: the map's HUD sliding down out of the picture, see drawIsoFlight)
			const int y = surface->getY();
			surface->setY(y + (int)std::lround(_isoHudSlide * Options::baseYResolution));
			surface->blit(screen);
			surface->setY(y);
			continue;
		}
		if (isoShown && surface == _state->_map)
		{
			_state->_map->draw(); // (drawn, not shown: its overlay pass is 3DIso's, see Map::setOverlayTarget)
			continue;
		}
		if (!_ownSurfaces.count(surface) && (!full3D || surface == _state->_warning || surface == _state->_txtDebug))
		{
			surface->blit(screen);
		}
	}

	// Our overlays (see drawOverlays). VR mode with the shell drawing the eyes: the HUD's own
	// picture too, for its quad layer.
	drawOverlays(screen, false, composing);
	if (shellDrawsEyes)
	{
		vrComposeHud();
	}
	return true;
}

/**
 * Our overlays onto a surface the screen's size: straight onto it, or onto the cleared canvas and
 * magnified (its empty pixels let what's under them show); with them, when the classic HUD that
 * normally has them is hidden (and the 3D HUD isn't), the original's visible enemy buttons. For
 * VR's HUD picture (see vrComposeHud), without what the eyes draw themselves at its depth: the
 * crosshair and its accuracy line, the zoom's scope cross, and the weapon in hand (a 3D object in the
 * eyes, setting "VR weapon").
 * With the canvas on the display (setting "3D resolution": Display) the screen's frame is put
 * together here instead: the view with the overlays magnified over it, laid into the display when
 * the engine shows the frame (see FirstPerson::presentView).
 * @param target The screen, or the HUD's picture.
 * @param forHud True for VR's HUD picture.
 * @param composing True to put the display's frame together (see blit); never for the HUD's picture.
 */
void FirstPersonMode::drawOverlays(SDL_Surface *target, bool forHud, bool composing)
{
	const bool full3D = isActive() && !_classicHud;
	const bool enemyButtons = full3D && !hudHidden() && !_feedOverlays;
	auto shown = [&](const Surface *surface)
	{
		return surface != _view && surface != _isoView && _ownSurfaces.count(const_cast<Surface*>(surface)) && !(hudHidden() && _topBarSurfaces.count(const_cast<Surface*>(surface)))
			&& (!_feedOverlays || feedOverlay(surface)) // (the Live command feed shows only some, see feedOverlay)
			&& !(forHud && (surface == _crosshair || surface == _accuracyLine || surface == _scopeCross || surface == _weaponInHand));
	};
	// (3DIso's flight just landed in the soldier's eyes: the 3D HUD comes in as the weapon comes up, see IsoLandRaise: what's
	// low on the screen slides up from under its bottom edge, what's high down from over its top; the top bar, there all along,
	// and the crosshair, its marks and its labels round the screen's middle, the aim, stay)
	const Uint32 now = SDL_GetTicks();
	const double landing = _isoLandTick && now - _isoLandTick < IsoLandRaise ? 1.0 - (double)(now - _isoLandTick) / IsoLandRaise : 0.0;
	const double away = landing * landing * (3.0 - 2.0 * landing);
	auto blitShown = [&](Surface *surface, SDL_Surface *into) {
		const int middleX = surface->getX() + surface->getWidth() / 2, middleY = surface->getY() + surface->getHeight() / 2;
		const bool aimed = std::abs(middleX - into->w / 2) < into->w / 6 && std::abs(middleY - into->h / 2) < into->h / 6;
		if (away <= 0.0 || aimed || _topBarSurfaces.count(surface) || surface == _weaponInHand || surface == _crosshair || surface == _accuracyLine || surface == _scopeCross)
		{
			surface->blit(into);
			return;
		}
		const int y = surface->getY();
		const bool low = y + surface->getHeight() / 2 > into->h / 2;
		surface->setY(y + (int)std::lround(away * (low ? into->h - y : -(y + surface->getHeight()))));
		surface->blit(into);
		surface->setY(y);
	};
	if (!_canvas)
	{
		for (Surface *surface : _state->_surfaces)
		{
			if (shown(surface)) blitShown(surface, target);
		}
		if (enemyButtons) drawEnemyButtons(target);
	}
	else
	{
		_canvas->clear();
		for (Surface *surface : _state->_surfaces)
		{
			if (shown(surface)) blitShown(surface, _canvas->getSurface());
		}
		if (enemyButtons) drawEnemyButtons(_canvas->getSurface());
		if (!composing)
		{
			magnify(target, _canvas->getSurface(), true);
			return;
		}

		// The canvas on the display: the frame's picture is the view with the overlays magnified
		// over it at their own scale, the display's size, and that is what is laid into the
		// display under whatever the game's screen holds (its warnings, windows and pointer).
		SDL_Surface *picture = _composed->getSurface();
		if (_view->shown() && _view->getWidth() == picture->w && _view->getHeight() == picture->h)
		{
			SDL_Surface *seen = _view->getSurface();
			for (int row = 0; row < picture->h; ++row)
			{
				std::memcpy((Uint8 *)picture->pixels + (size_t)row * picture->pitch, (const Uint8 *)seen->pixels + (size_t)row * seen->pitch, picture->w);
			}
		}
		else
		{
			SDL_FillRect(picture, nullptr, 0); // (no view this frame: the palette's first colour, black)
		}
		magnify(picture, _canvas->getSurface(), true);
		_composed->setPalette(_state->getPalette());
		// In full colour (setting "True colour"): each pixel that still has the palette colour its ray gave it (no unit,
		// effect or wash over it, no overlay) takes the card's full colour for it; the others stay the palette's. But
		// see-through smoke drawn over a ray's pixel takes the light the card found in the air there (setting "Light
		// shafts": what the full colour has over the ray's palette colour), so a beam lights the smoke it crosses.
		// A pixel the overlays drew (the weapon in hand, the HUD) is theirs whatever its colour: told by the canvas, not
		// by the colour having changed. (The owner, with True colour: "when walking in gpu renderer my weapon in first
		// person flashes". Where a pixel of the gun had the very palette colour of the ground behind it, it counted as
		// untouched and took the ground's full colour, lit as the ground is: specks on the gun that came and went
		// with every step.)
		const SDL_Surface *canvas = _canvas->getSurface();
		const int uiScale = std::max(1, _uiScale);
		auto overlayRow = [canvas, uiScale](int row) -> const Uint8 * {
			return row / uiScale < canvas->h ? (const Uint8 *)canvas->pixels + (size_t)(row / uiScale) * canvas->pitch : nullptr;
		};
		auto overlaid = [canvas, uiScale](const Uint8 *over, int column) { return over && column / uiScale < canvas->w && over[column / uiScale] != 0; };
		const Uint32 *fullColors = nullptr, *shownInFull = nullptr;
		const Uint8 *rayColors = nullptr, *smokeVeil = nullptr;
		const Uint32 *cardPicture = nullptr;
		if (_view->shown() && _view->getWidth() == picture->w && _view->getHeight() == picture->h && _view->cardPicture(cardPicture))
		{
			// The frame as the graphics card put it together (see FirstPersonView::gpuPresent): the view's own surface holds
			// only what was drawn last; wherever the overlays here didn't draw over that, the card's pixel.
			SDL_Surface *seen = _view->getSurface();
			_truePicture.resize((size_t)picture->w * picture->h);
			for (int row = 0; row < picture->h; ++row)
			{
				const Uint8 *made = (const Uint8 *)picture->pixels + (size_t)row * picture->pitch, *own = (const Uint8 *)seen->pixels + (size_t)row * seen->pitch;
				const Uint32 *card = cardPicture + (size_t)row * picture->w;
				Uint32 *out = &_truePicture[(size_t)row * picture->w];
				const Uint8 *over = overlayRow(row);
				for (int column = 0; column < picture->w; ++column) out[column] = made[column] == own[column] && !overlaid(over, column) ? card[column] | 0xFF000000u : 0u;
			}
			shownInFull = _truePicture.data();
		}
		else if (_view->shown() && _view->getWidth() == picture->w && _view->getHeight() == picture->h && _view->trueColor(fullColors, rayColors, smokeVeil))
		{
			const SDL_Color *palette = _state->getPalette();
			_truePicture.resize((size_t)picture->w * picture->h);
			for (int row = 0; row < picture->h; ++row)
			{
				const Uint8 *made = (const Uint8 *)picture->pixels + (size_t)row * picture->pitch, *rays = rayColors + (size_t)row * picture->w;
				const Uint32 *full = fullColors + (size_t)row * picture->w;
				Uint32 *out = &_truePicture[(size_t)row * picture->w];
				const Uint8 *over = overlayRow(row);
				for (int column = 0; column < picture->w; ++column)
				{
					if (overlaid(over, column) || _view->drawnOver((size_t)row * picture->w + column))
					{
						out[column] = 0u;
					}
					else if (made[column] == rays[column])
					{
						out[column] = full[column] | 0xFF000000u;
					}
					else if (smokeVeil && smokeVeil[(size_t)row * picture->w + column])
					{
						const SDL_Color &smoke = palette[made[column]], &ray = palette[rays[column]];
						const int red = (int)((full[column] >> 16) & 0xFF) - ray.r, green = (int)((full[column] >> 8) & 0xFF) - ray.g, blue = (int)(full[column] & 0xFF) - ray.b;
						out[column] = 0xFF000000u | (Uint32)std::min(255, smoke.r + std::max(0, red)) << 16 | (Uint32)std::min(255, smoke.g + std::max(0, green)) << 8
							| (Uint32)std::min(255, smoke.b + std::max(0, blue));
					}
					else
					{
						out[column] = 0u;
					}
				}
			}
			shownInFull = _truePicture.data();
		}
		FirstPerson::presentView(_composed, shownInFull);
	}
}

/**
 * Where a visible enemy button goes in full 3D (see drawEnemyButtons): the column of those showing (the game shows
 * the first ones), stacked as the game stacks them (the first at the bottom, each EnemyButtonStep above the one
 * before), centred on the view's height, under the compass's middle (which keeps it clear of the red arrows that
 * mark enemies past the view's right edge); on a low screen under the compass's lines (turning, SPRINT, EN
 * PASSANT), and all of it on the screen.
 * @param index The button (0: the first).
 * @param x Its left edge, in canvas pixels.
 * @param y Its top, in canvas pixels.
 */
void FirstPersonMode::enemyButtonPlace(int index, int &x, int &y) const
{
	const InteractiveSurface *button = _state->_btnVisibleUnit[index];
	int shown = 1;
	while (shown < BattlescapeState::VISIBLE_MAX && _state->_btnVisibleUnit[shown]->getVisible()) ++shown;
	const int columnHeight = (shown - 1) * EnemyButtonStep + button->getHeight();
	const int underCompass = _txtCompassTurn->getY() + _txtCompassTurn->getHeight() + 1 + _txtSprint->getHeight() + _txtEnPassant->getHeight() + EnemyButtonsGap;
	const int top = std::max(0, std::min(std::max(canvasHeight() / 2 - columnHeight / 2, underCompass), canvasHeight() - 2 - columnHeight));
	x = _compass->getX() + CompassSize / 2 - button->getWidth() / 2;
	y = top + (shown - 1 - index) * EnemyButtonStep;
}

/**
 * Is a point on the canvas on a visible enemy button, where full 3D draws it (see enemyButtonPlace)?
 * @param index The button.
 * @param canvasX The point, in canvas pixels.
 * @param canvasY The same, down.
 * @return True if the button shows there.
 */
bool FirstPersonMode::onEnemyButton(int index, double canvasX, double canvasY) const
{
	const InteractiveSurface *button = _state->_btnVisibleUnit[index];
	int x, y;
	enemyButtonPlace(index, x, y);
	return button->getVisible() && canvasX >= x && canvasX < x + button->getWidth() && canvasY >= y && canvasY < y + button->getHeight();
}

/**
 * The original's visible enemy buttons (clicking one turns the view to it), with their numbers, in full 3D: in a
 * column at the right, centred on the view's height (a player: where the classic HUD has them they stood off to the
 * middle of a wide screen), at the HUD's scale (drawn with our overlays). Each is moved there for the frame and put
 * back, so the game's HUD keeps them where it has them (the map, the classic HUD).
 * @param target The canvas, or the screen when there's no canvas (scale 1).
 */
void FirstPersonMode::drawEnemyButtons(SDL_Surface *target)
{
	for (int enemyIndex = 0; enemyIndex < BattlescapeState::VISIBLE_MAX; ++enemyIndex)
	{
		InteractiveSurface *button = _state->_btnVisibleUnit[enemyIndex];
		NumberText *number = _state->_numVisibleUnit[enemyIndex];
		if (!button->getVisible()) continue;
		const int buttonX = button->getX(), buttonY = button->getY(), numberX = number->getX(), numberY = number->getY();
		int x, y;
		enemyButtonPlace(enemyIndex, x, y);
		button->setX(x);
		button->setY(y);
		number->setX(x + numberX - buttonX); // (the number where the game puts it on its button)
		number->setY(y + numberY - buttonY);
		button->blit(target);
		number->blit(target);
		button->setX(buttonX);
		button->setY(buttonY);
		number->setX(numberX);
		number->setY(numberY);
	}
}

/**
 * Copies a canvas-sized surface (the canvas with the overlays, or the 3D view itself) onto the
 * screen _uiScale times bigger, each of its pixels becoming a square of _uiScale x _uiScale
 * screen pixels. The sources are 8-bit surfaces in the state's palette: on an 8-bit screen their
 * colours are copied as they are, on a 32-bit one (the Android build's screen always is) each
 * goes through the palette.
 * @param screen The screen surface to draw on.
 * @param source The surface to magnify, the canvas's size.
 * @param transparent True to skip the source's empty pixels (colour 0), so what's on the screen
 * shows through them; false to copy every pixel.
 */
void FirstPersonMode::magnify(SDL_Surface *screen, SDL_Surface *source, bool transparent)
{
	// Keep the canvas's palette the same as the state's (it changes, e.g. with the time of day).
	const SDL_Color *palette = _state->getPalette();
	if (!std::equal(palette, palette + 256, _canvasPalette, [](const SDL_Color &stateColor, const SDL_Color &canvasColor)
		{ return stateColor.r == canvasColor.r && stateColor.g == canvasColor.g && stateColor.b == canvasColor.b; }))
	{
		std::copy(palette, palette + 256, _canvasPalette);
		_canvas->setPalette(_canvasPalette);
	}

	// The part of the screen the magnified surface covers (it can be a few pixels short of the
	// screen, when the screen isn't a multiple of the scale).
	const int drawWidth = std::min(screen->w, source->w * _uiScale), drawHeight = std::min(screen->h, source->h * _uiScale);
	SDL_LockSurface(screen);
	SDL_LockSurface(source);

	// A 32-bit screen's colour for each of the source's 256 (none needed for an 8-bit screen).
	const bool trueColorScreen = screen->format->BytesPerPixel == 4;
	std::array<Uint32, 256> screenColors{};
	if (trueColorScreen)
	{
		const SDL_Palette *sourcePalette = source->format->palette;
		for (int index = 0; sourcePalette && index < std::min(256, sourcePalette->ncolors); ++index)
		{
			const SDL_Color &color = sourcePalette->colors[index];
			screenColors[index] = SDL_MapRGB(screen->format, color.r, color.g, color.b);
		}
	}

	// Each source row is widened once, then laid on its _uiScale screen rows (copied whole
	// where opaque).
	_magnifyRow.resize(drawWidth);
	const int sourceColumns = (drawWidth + _uiScale - 1) / _uiScale; // source pixels per row that reach the screen
	for (int sourceY = 0; sourceY * _uiScale < drawHeight; ++sourceY)
	{
		// The part of the row to lay on the screen: all of it when opaque; when transparent only
		// from its first to its last coloured pixel, and nothing at all for an empty row (over
		// the 3D view most overlay rows are empty, or nearly: the crosshair, a line of text).
		const Uint8 *sourceRow = (const Uint8 *)source->pixels + sourceY * source->pitch;
		int firstColumn = 0, endColumn = sourceColumns;
		if (transparent)
		{
			while (firstColumn < endColumn && !sourceRow[firstColumn]) ++firstColumn;
			while (endColumn > firstColumn && !sourceRow[endColumn - 1]) --endColumn;
			if (firstColumn == endColumn)
			{
				continue;
			}
		}
		const int firstScreenX = firstColumn * _uiScale, endScreenX = std::min(drawWidth, endColumn * _uiScale);

		// Widen that part: every source pixel repeated _uiScale times (on most big screens twice,
		// done in pairs, which the compiler makes quick; then whatever is left one by one).
		int widenedX = firstScreenX, sourceX = firstColumn;
		if (_uiScale == 2)
		{
			for (; widenedX + 2 <= endScreenX; widenedX += 2, ++sourceX) _magnifyRow[widenedX] = _magnifyRow[widenedX + 1] = sourceRow[sourceX];
		}
		for (; widenedX < endScreenX; ++sourceX)
		{
			for (int repeat = 0; repeat < _uiScale && widenedX < endScreenX; ++repeat) _magnifyRow[widenedX++] = sourceRow[sourceX];
		}

		// Lay it on each of its screen rows: all of it, or only its coloured pixels.
		for (int screenY = sourceY * _uiScale; screenY < std::min(drawHeight, (sourceY + 1) * _uiScale); ++screenY)
		{
			// A 32-bit screen: each pixel in its colour (from the first to the last coloured
			// pixel when transparent; that's the whole row when opaque).
			if (trueColorScreen)
			{
				Uint32 *trueColorRow = (Uint32 *)((Uint8 *)screen->pixels + screenY * screen->pitch);
				for (int screenX = firstScreenX; screenX < endScreenX; ++screenX)
				{
					if (!transparent || _magnifyRow[screenX]) trueColorRow[screenX] = screenColors[_magnifyRow[screenX]];
				}
				continue;
			}
			Uint8 *screenRow = (Uint8 *)screen->pixels + screenY * screen->pitch;
			if (!transparent)
			{
				std::copy(_magnifyRow.begin(), _magnifyRow.end(), screenRow);
				continue;
			}
			for (int screenX = firstScreenX; screenX < endScreenX; ++screenX)
			{
				if (_magnifyRow[screenX]) screenRow[screenX] = _magnifyRow[screenX];
			}
		}
	}
	SDL_UnlockSurface(source);
	SDL_UnlockSurface(screen);
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.

namespace OpenXcom
{

/**
 * 3DIso (research, only with the environment variable OXC3D_ISO3D: its value the mode to start in, 0 or 1): the
 * isometric map drawn by the 3D view with the map's own camera (FirstPersonView::setIsoCamera), laid over the map.
 * The key "3DIso or map" switches between the two. With OXC3D_ISO3D_DUMP a folder: a file "go" in it makes the next frame write the map's
 * picture (iso.bmp) and the 3D view's (iso3d.bmp) of the same moment, then removes "go"; a file "mode" sets the mode.
 */
void FirstPersonMode::updateIsoView()
{
	static const char *startMode = getenv("OXC3D_ISO3D");
	static const char *dumpFolder = getenv("OXC3D_ISO3D_DUMP");
	// (the setting "3DIso", or a test's OXC3D_ISO3D: its starting mode, 1 3DIso, 0 the map; the key "3DIso or map" switches)
	if (!startMode && !Options::fpsIso)
	{
		FirstPerson::setIsoFullColour(false);
		if (_isoView && _isoView->getVisible())
		{
			// (switched off in the options: the map as ever)
			_isoView->setVisible(false);
			_state->_map->setOverlayTarget(nullptr);
			_state->_map->setSelectorTransform(nullptr);
			_state->_map->setOverlayLeaves(false, false, false);
			FirstPerson::setIsoFullColour(false);
		}
		return;
	}
	if (_isoMode < 0) _isoMode = startMode ? (std::atoi(startMode) ? 1 : 0) : 1;
	// The extras: the settings' (see settingsIsoExtras), or for a test OXC3D_ISO3D_EXTRAS's words (markers, downed, light,
	// effects, shots, smoke, colouredlight, facing, voxel, outlines, display, truecolour, turn, layer); F9 switches them off and on.
	static const char *extrasAsked = getenv("OXC3D_ISO3D_EXTRAS");
	if (!extrasAsked)
	{
		const bool effects = Options::fpsIsoEffects, display = Options::fpsIsoResolution == 1;
		_isoExtras = FirstPersonView::IsoExtraTurn | (display ? FirstPersonView::IsoExtraDisplay : 0) | (display && Options::fpsTrueColor ? FirstPersonView::IsoExtraTrueColour : 0);
		if (effects)
		{
			// (off: as on the map; on: the 3D view's effects and its settings' marks and light)
			_isoExtras |= FirstPersonView::IsoExtraEffects | FirstPersonView::IsoExtraShots | FirstPersonView::IsoExtraSmoke | FirstPersonView::IsoExtraLayer
				| (Options::fpsSmoothLighting ? FirstPersonView::IsoExtraSmoothLight : 0)
				| (Options::fpsEnemyMarkers > 0 || Options::fpsWoundedMarkers > 0 ? FirstPersonView::IsoExtraMarkers : 0)
				| (Options::fpsDownedIcons > 0 ? FirstPersonView::IsoExtraDowned : 0)
				| (Options::fpsVoxelUnits ? FirstPersonView::IsoExtraVoxelUnits : 0)
				| (Options::fpsHitboxes > 0 ? FirstPersonView::IsoExtraOutlines : 0)
				| (Options::fpsColoredLights ? FirstPersonView::IsoExtraColouredLight : 0)
				| (Options::fpsIsoWeather ? FirstPersonView::IsoExtraWeather : 0);
		}
	}
	else if (_isoExtras < 0)
	{
		const char *asked = extrasAsked;
		const std::string list = asked ? asked : "markers,downed,light,effects,shots,smoke,colouredlight,turn"; // (facing: the map shows its own arrows with Alt)
		static const std::pair<const char*, int> words[] = { { "markers", FirstPersonView::IsoExtraMarkers }, { "downed", FirstPersonView::IsoExtraDowned },
			{ "light", FirstPersonView::IsoExtraSmoothLight }, { "effects", FirstPersonView::IsoExtraEffects }, { "shots", FirstPersonView::IsoExtraShots },
			{ "smoke", FirstPersonView::IsoExtraSmoke }, { "facing", FirstPersonView::IsoExtraFacing }, { "voxel", FirstPersonView::IsoExtraVoxelUnits },
			{ "outlines", FirstPersonView::IsoExtraOutlines }, { "colouredlight", FirstPersonView::IsoExtraColouredLight },
			{ "display", FirstPersonView::IsoExtraDisplay }, { "truecolour", FirstPersonView::IsoExtraTrueColour }, { "turn", FirstPersonView::IsoExtraTurn },
			{ "layer", FirstPersonView::IsoExtraLayer }, { "weather", FirstPersonView::IsoExtraWeather } };
		_isoExtras = 0;
		std::stringstream named;
		named << list;
		for (std::string word; std::getline(named, word, ',');)
			for (const auto &known : words)
				if (word == known.first) _isoExtras |= known.second;
	}
	Map *map = _state->_map;
	// (extra "display": the display's own resolution, laid into it under the HUD, see blit; else the map's. "true colour" with
	// it: the display in full colour, see FirstPerson::wantsFullColorDisplay)
	const bool isoDisplay = _isoExtrasOn && (_isoExtras & FirstPersonView::IsoExtraDisplay);
	FirstPerson::setIsoFullColour(isoDisplay && (_isoExtras & FirstPersonView::IsoExtraTrueColour) && _isoMode == 1);
	if (_isoView) _isoView->setWantsTrueColor(isoDisplay && (_isoExtras & FirstPersonView::IsoExtraTrueColour)); // (on the card: its full colour, coloured lights, smooth light)
	int isoWidth = map->getWidth(), isoHeight = map->getHeight();
	if (isoDisplay && FirstPerson::canPresentAtDisplaySize())
	{
		// (the picture is scaled alike both ways, see setIsoScale: only a display picture of the map's shape, square pixels)
		int pictureX, pictureY, pictureWidth, pictureHeight;
		_game->getScreen()->getPictureArea(pictureX, pictureY, pictureWidth, pictureHeight);
		if (pictureWidth > 0 && pictureHeight > 0 && std::abs(pictureWidth * map->getHeight() - pictureHeight * map->getWidth()) <= map->getWidth())
		{
			isoWidth = pictureWidth;
			isoHeight = pictureHeight;
		}
	}
	if (!_isoView || _isoView->getWidth() != isoWidth || _isoView->getHeight() != isoHeight)
	{
		FirstPersonView *view = new FirstPersonView(_game, isoWidth, isoHeight, map->getX(), map->getY());
		view->setPalette(_state->getPalette());
		view->shareCache(_view);
		view->useVoxelUnits(_voxelUnits.get());
		view->setSavedBattleGame(_save);
		view->setMap(map);
		view->setParticles(false);
		view->setComfort(true);
		view->setSmoothLighting(false);
		if (_isoView)
		{
			map->setSelectorTransform(nullptr); // (it may point at the old view; set again below)
			view->takeFlares(*_isoView); // (its flares and marks go on)
			replaceStateSurface(_isoView, view);
			_ownSurfaces.erase(_isoView);
			delete _isoView;
		}
		else
		{
			_state->add(view);
			auto &surfaces = _state->_surfaces;
			surfaces.pop_back();
			surfaces.insert(std::find(surfaces.begin(), surfaces.end(), map) + 1, view);
		}
		_ownSurfaces.insert(view);
		_isoView = view;
		map->setOverlayTarget(nullptr); // (the map draws into it: not once it's gone)
		delete _isoOverlay;
		_isoOverlay = new Surface(map->getWidth(), map->getHeight(), 0, 0);
		_isoView->setIsoOverlay(_isoOverlay);
	}
	// (3DIso's switches act on the key's press, see handleKey; its camera keys are held, read here)
	// (3DIso's keys, the player's: see the settings' Controls; none set: never held. SDL 1.2's and SDL2's alike, see isKeyHeld)
	auto held = [](SDL_Keycode key) { return key != SDLK_UNKNOWN && isKeyHeld(key); };
	// Our 3D art or the original art (F11, or the top bar's A/R button; OXC3D_ISO3D_ART=original starts on the original).
	static const char *artAsked = getenv("OXC3D_ISO3D_ART");
	if (_isoArt3D < 0 || (!artAsked && Options::fpsIsoArt != _isoArtSetting))
	{
		// (the setting "3DIso art", read again when the options change it; a test's OXC3D_ISO3D_ARTs)
		_isoArt3D = artAsked ? (std::string(artAsked) == "original" ? 0 : 1) : (Options::fpsIsoArt == 1 ? 0 : 1);
		_isoArtSetting = Options::fpsIsoArt;
	}
	const bool flareKey = held(Options::keyFpsFlare);
	if (flareKey && !_isoFlareKeyDown && _isoMode == 1 && !isActive() && !_isoFlight.on) throwIsoFlare();
	_isoFlareKeyDown = flareKey;
	const int extrasNow = _isoExtrasOn ? _isoExtras : 0;
	_isoView->setIsoExtras(extrasNow);
	_isoView->setParticles((extrasNow & FirstPersonView::IsoExtraEffects) != 0); // (its own sparks, blood, debris)
	_isoView->setShowFacing((extrasNow & FirstPersonView::IsoExtraFacing) && isKeyHeld(Options::keyFpsShowFacing));
	// (the map's items, smoke and fire left out of its overlay when 3DIso draws them: with its 3D effects, or its art smooth)
	const bool smoothSprites = _isoView->isoMapSpritesSmooth();
	_isoView->setIsoMapSpritesNow(smoothSprites); // (the same for the map's overlay and the view this frame)
	map->setOverlayLeaves((extrasNow & FirstPersonView::IsoExtraShots) != 0, (extrasNow & FirstPersonView::IsoExtraSmoke) != 0 || smoothSprites,
		(extrasNow & FirstPersonView::IsoExtraLayer) != 0, smoothSprites);
	if (!_isoFlight.on) _isoView->setSmoothLighting(_isoExtrasOn && (_isoExtras & FirstPersonView::IsoExtraSmoothLight)); // (lightAt blends between tiles only so)
	if (_isoExtrasOn && (_isoExtras & FirstPersonView::IsoExtraMarkers)) updateEnemyMarkers();
	{
		// The zoom, + or - held: smoothly, 1 to 4 times the map's own size, about the map's middle (sharper with the extra
		// "display"); the map's cursor and clicks follow the picture (see Map::setSelectorTransform).
		const Uint32 ticks = SDL_GetTicks();
		const double seconds = _isoZoomTick ? std::min(0.1, (ticks - _isoZoomTick) / 1000.0) : 0.0;
		_isoZoomTick = ticks;
		_isoFrameSeconds = seconds;
		// (the camera's keys only while 3DIso shows: in 3D they're the 3D view's)
		const bool cameraKeys = _isoMode == 1 && !isActive() && !_isoFlight.on;
		if (cameraKeys && _isoShot.phase == 0) // (the shot camera has the zoom while it follows a shot)
		{
			if (held(Options::keyFpsIsoZoomIn) || held(SDLK_KP_PLUS)) _isoZoomTarget = std::min(4.0, _isoZoomTarget * std::pow(2.0, seconds * 1.5));
			if (held(Options::keyFpsIsoZoomOut) || held(SDLK_KP_MINUS)) _isoZoomTarget = std::max(isoMinZoom(), _isoZoomTarget / std::pow(2.0, seconds * 1.5));
		}
		// (the reset key: back to the map's own side, slant and size)
		if (_isoResetAsked && cameraKeys && _isoShot.phase == 0)
		{
			_isoZoomTarget = 1.0;
			_isoTurnTarget = std::round(_isoTurnNow / 360.0) * 360.0; // (the nearest whole turn: the short way round)
			_isoTiltTarget = 0.0;
		}
		_isoResetAsked = false;
		// The shot camera (setting "3DIso shot camera"): a shot zoomed in on (1.6 times, up to 3) and followed, its blasts held
		// a moment (IsoShotHold), then back to the zoom it set off from and the map's offset; a shot after it carries on.
		IsoShotCamera &shotCamera = _isoShot;
		{
			// (only a shot the map shows and would follow: not one out of sight, an unseen alien's; not behind HIDDEN MOVEMENT;
			// not with following off, Alt held or the weapon's own rule)
			const Projectile *shot = map->getProjectile() && map->isProjectileInFOV() && map->getFollowProjectile() ? map->getProjectile() : nullptr;
			const bool followed = Options::fpsIsoShotCamera && _isoMode == 1 && !isActive() && !_isoFlight.on && !map->showsHiddenMovement();
			const Uint32 IsoShotHold = 350;
			if (!followed && shotCamera.phase != 0)
			{
				// (3D, the flight, the map or the setting off: back at once)
				_isoZoomTarget = shotCamera.zoomBack;
				shotCamera = IsoShotCamera();
			}
			else if (followed && shot)
			{
				if (shotCamera.phase == 0) shotCamera.zoomBack = _isoZoomTarget;
				shotCamera.phase = 1;
				Position screen;
				map->getCamera()->convertVoxelToScreen(shot->getPosition(), &screen);
				const Position offset = map->getCamera()->getMapOffset();
				shotCamera.aim[0] = screen.x - offset.x;
				shotCamera.aim[1] = screen.y - offset.y;
				_isoZoomTarget = std::max(shotCamera.zoomBack, std::min(3.0, shotCamera.zoomBack * 1.6));
			}
			else if (shotCamera.phase == 1 || shotCamera.phase == 2)
			{
				if (shotCamera.phase == 1 || !map->getExplosions()->empty()) shotCamera.since = ticks;
				shotCamera.phase = ticks - shotCamera.since > IsoShotHold ? 3 : 2;
				if (shotCamera.phase == 3) _isoZoomTarget = shotCamera.zoomBack;
			}
		}
		_isoZoom += (_isoZoomTarget - _isoZoom) * std::min(1.0, seconds * 12.0);
		if (std::abs(_isoZoom - _isoZoomTarget) < 1e-3) _isoZoom = _isoZoomTarget;
		_isoView->setIsoScale(map->getWidth(), map->getHeight(), (double)_isoView->getWidth() / map->getWidth() * _isoZoom);
		// Smooth scrolling: the picture follows the map's own offset (whole pixels, in steps as the map scrolls, at once to a unit
		// it centres on, a level's height on a level change) gliding, a little behind it (see FirstPersonView::setIsoShift).
		{
			const Position actual = map->getCamera()->getMapOffset();
			const double target[2] = { (double)actual.x, (double)actual.y };
			if (!_isoShownSet) { _isoShown[0] = target[0]; _isoShown[1] = target[1]; _isoShownSet = true; }
			for (int axis = 0; axis < 2; ++axis)
			{
				_isoShown[axis] += (target[axis] - _isoShown[axis]) * std::min(1.0, seconds * 14.0);
				if (std::abs(target[axis] - _isoShown[axis]) < 0.02) _isoShown[axis] = target[axis];
			}
			// (a big explosion shakes the picture a moment, as the 3D view's camera shakes: setting "Screen shake"; the map flashes)
			double shake[2] = { 0.0, 0.0 };
			if (Options::fpsScreenShake)
			{
				bool big = false;
				for (const Explosion *explosion : *map->getExplosions()) big = big || (explosion->isBig() && explosion->getCurrentFrame() >= 0 && explosion->getCurrentFrame() <= 1); // (its first frames shown: a delayed one waits below 0)
				if (big && ticks - _isoShakeStart > 300) _isoShakeStart = ticks;
				const double IsoShakeTime = 400.0, since = ticks - _isoShakeStart;
				if (_isoShakeStart && since < IsoShakeTime)
				{
					const double strength = 3.0 * (1.0 - since / IsoShakeTime);
					shake[0] = strength * std::sin(since * 0.11);
					shake[1] = strength * std::cos(since * 0.083);
				}
			}
			// (the shot camera's move: the picture's middle on the shot's point, gliding there and back; the map's own offset
			// may scroll meanwhile, the point stays put on the map)
			for (int axis = 0; axis < 2; ++axis)
			{
				const double middle = (axis == 0 ? map->getWidth() : map->getHeight() - map->getIconHeight()) / 2.0; // (the map over the HUD's middle)
				const double wanted = shotCamera.phase == 1 || shotCamera.phase == 2 ? shotCamera.aim[axis] - middle + _isoShown[axis] : 0.0;
				shotCamera.pan[axis] += (wanted - shotCamera.pan[axis]) * std::min(1.0, seconds * (shotCamera.phase == 3 ? 6.0 : 9.0));
				if (std::abs(wanted - shotCamera.pan[axis]) < 0.05) shotCamera.pan[axis] = wanted;
			}
			if (shotCamera.phase == 3 && shotCamera.pan[0] == 0.0 && shotCamera.pan[1] == 0.0 && std::abs(_isoZoom - _isoZoomTarget) < 1e-3) shotCamera.phase = 0;
			_isoView->setIsoShift(target[0] - _isoShown[0] + shake[0] + shotCamera.pan[0], target[1] - _isoShown[1] + shake[1] + shotCamera.pan[1]);
		}
		// Extra "turn": [ and ] turn 3DIso a quarter about the view's middle, Shift+[ and Shift+] tilt it 15 degrees flatter or
		// steeper (from 10 to 80 degrees down), smoothly; drawn in 3D (see FirstPersonView::setIsoTurn).
		const bool turnExtra = _isoExtrasOn && (_isoExtras & FirstPersonView::IsoExtraTurn);
		const bool turning = turnExtra && !_isoFlight.on;
		if (_isoFree && (!turnExtra || _isoMode != 1)) setIsoFree(false); // (the free camera only where 3DIso turns; kept in 3D and back)
		const bool shifted = (SDL_GetModState() & KMOD_SHIFT) != 0;
		const bool turnLeftKey = cameraKeys && held(Options::keyFpsIsoTurnLeft), turnRightKey = cameraKeys && held(Options::keyFpsIsoTurnRight);
		const bool turnLeft = turnLeftKey && !shifted, turnRight = turnRightKey && !shifted;
		const bool tiltFlatter = cameraKeys && (held(Options::keyFpsIsoTiltFlatter) || (turnLeftKey && shifted));
		const bool tiltSteeper = cameraKeys && (held(Options::keyFpsIsoTiltSteeper) || (turnRightKey && shifted));
		if (turning && !_isoTurnKeyDown)
		{
			if (tiltFlatter) _isoTiltTarget = std::max(_isoFree ? -20.0 : -15.0, _isoTiltTarget - 15.0); // (in steps of 15 from the map's slant: 15 to 75 degrees down)
			if (tiltSteeper) _isoTiltTarget = std::min(_isoFree ? 50.0 : 45.0, _isoTiltTarget + 15.0);
			if (turnLeft) _isoTurnTarget -= 90.0;
			if (turnRight) _isoTurnTarget += 90.0;
		}
		if (!turnExtra)
		{
			// (the extra off: back to the map's own side the short way, and its slant)
			_isoTurnTarget = std::round(_isoTurnNow / 360.0) * 360.0;
			_isoTiltTarget = 0.0;
		}
		// Our 3D art and the original art cross-fade live (IsoArtFadeTime): on switching, and on the original art around a turn
		// or a tilt: the art fades into 3D where it is first, then the camera moves; back on the map's slant, the 3D fades into
		// the art (see FirstPersonView::setIsoVoxelShare).
		{
			const double IsoArtFadeTime = 250.0;
			const bool leaving = _isoTurnTarget != 0.0 || _isoTiltTarget != 0.0 || _isoTurnNow != 0.0 || _isoTiltNow != 0.0;
			const double shareTarget = _isoArt3D != 0 || leaving ? 1.0 : 0.0;
			const double step = _isoFrameSeconds * 1000.0 / IsoArtFadeTime;
			_isoVoxelShare = shareTarget > _isoVoxelShare ? std::min(shareTarget, _isoVoxelShare + step) : std::max(shareTarget, _isoVoxelShare - step);
		}
		// (the free camera let go while moving: it glides on, slowing down over a quarter of a second)
		if (_isoFree && !_isoFreeDrag.button && seconds > 0.0)
		{
			double *speed = _isoFreeVelocity;
			if (speed[0] != 0.0 || speed[1] != 0.0)
			{
				_isoTurnTarget += speed[0] * seconds;
				_isoTiltTarget = std::clamp(_isoTiltTarget + speed[1] * seconds, -20.0, 50.0);
			}
			if (speed[2] != 0.0 || speed[3] != 0.0)
			{
				double moveX = speed[2] * seconds, moveY = speed[3] * seconds;
				isoMoveOnMap(_isoTurnNow, _isoTiltNow, _isoZoom, moveX, moveY);
				_isoFreeGlidePan[0] += moveX;
				_isoFreeGlidePan[1] += moveY;
				const int stepX = (int)_isoFreeGlidePan[0], stepY = (int)_isoFreeGlidePan[1];
				if (stepX || stepY) map->getCamera()->scrollXY(stepX, stepY, true, true);
				_isoFreeGlidePan[0] -= stepX;
				_isoFreeGlidePan[1] -= stepY;
			}
			const double slowing = std::exp(-seconds / 0.25);
			for (int axis = 0; axis < 4; ++axis)
			{
				speed[axis] *= slowing;
				if (std::abs(speed[axis]) < (axis < 2 ? 1.0 : 4.0)) speed[axis] = 0.0;
			}
		}
		// (a new target, set anywhere since the last frame, eases from where the tilt and the turn are now; with the free camera
		// they follow the mouse's targets closely instead)
		const double IsoFreeFollow = 20.0;
		if (_isoTiltTarget != _isoTiltEased)
		{
			_isoTiltFrom = _isoTiltNow;
			_isoTiltStart = ticks;
			_isoTiltEased = _isoTiltTarget;
		}
		if (_isoFree) _isoTiltNow += (_isoTiltTarget - _isoTiltNow) * std::min(1.0, seconds * IsoFreeFollow);
		else
		{
			const double share = std::min(1.0, (ticks - _isoTiltStart) / 450.0);
			_isoTiltNow = share >= 1.0 ? _isoTiltTarget : _isoTiltFrom + (_isoTiltTarget - _isoTiltFrom) * share * share * (3.0 - 2.0 * share);
		}
		_isoView->setIsoTilt(_isoTiltNow);
		_isoTurnKeyDown = turnLeftKey || turnRightKey || tiltFlatter || tiltSteeper;
		if (_isoTurnTarget != _isoTurnEased)
		{
			// (each quarter turn eased in and out over IsoTurnTime, from wherever the turn is)
			_isoTurnFrom = _isoTurnNow;
			_isoTurnStart = ticks;
			_isoTurnEased = _isoTurnTarget;
		}
		if (_isoFree) _isoTurnNow += (_isoTurnTarget - _isoTurnNow) * std::min(1.0, seconds * IsoFreeFollow);
		else
		{
			const double IsoTurnTime = 450.0;
			const double share = std::min(1.0, (ticks - _isoTurnStart) / IsoTurnTime);
			_isoTurnNow = _isoTurnFrom + (_isoTurnTarget - _isoTurnFrom) * share * share * (3.0 - 2.0 * share);
			if (share >= 1.0)
			{
				_isoTurnTarget = std::fmod(_isoTurnTarget, 360.0);
				if (std::abs(_isoTurnTarget) < 1e-9 || std::abs(std::abs(_isoTurnTarget) - 360.0) < 1e-9) _isoTurnTarget = 0.0;
				_isoTurnNow = _isoTurnFrom = _isoTurnEased = _isoTurnTarget;
			}
		}
		_isoView->setIsoTurn(_isoTurnNow);
		// (the map's cursor and clicks go through 3DIso's picture only while it shows: on the map, in 3D or during a flight
		// they are the map's own)
		const bool pictureShown = _isoMode == 1 && !isActive() && !feedShown() && !map->showsHiddenMovement() && !_isoFlight.on;
		if (!pictureShown) map->setSelectorTransform(nullptr);
		else if (_isoTurnNow != 0.0 || _isoTiltNow != 0.0)
		{
			// (the tile the mouse is on in the turned picture, handed to the map as the point of its own picture over that tile:
			// the floor's middle, less the quarter tile the map adds, see Map::setSelectorPosition; the camera's offset and level
			// as they are when it's asked, which may be after a scroll since this frame)
			FirstPersonView *view = _isoView;
			const double toView = (double)_isoView->getWidth() / map->getWidth();
			Camera *mapCamera = map->getCamera();
			const int level = mapCamera->getViewLevel();
			map->setSelectorTransform([view, toView, mapCamera](int &x, int &y) {
				const Position offset = mapCamera->getMapOffset();
				const int viewLevel = mapCamera->getViewLevel();
				int tileX, tileY;
				if (!view->isoTileAt((x + 0.5) * toView, (y + 0.5) * toView, viewLevel, tileX, tileY)) { x = -10000; y = -10000; return; }
				x = 16 * (tileX - tileY) + offset.x + 16;
				y = 8 * (tileX + tileY) - 24 * viewLevel + offset.y + 32 - 10;
			});
			Position selected;
			map->getSelectorPosition(&selected);
			const bool cursorShown = map->getCursorType() != CT_NONE && !_state->getMouseOverIcons();
			_isoView->setIsoCursor(cursorShown ? selected.x : -1, selected.y, level);
		}
		else if (_isoZoom != 1.0 || _isoShown[0] != map->getCamera()->getMapOffset().x || _isoShown[1] != map->getCamera()->getMapOffset().y || _isoShot.phase != 0)
		{
			// (zoomed, or still gliding: the map's pixel under the mouse in the picture, see FirstPersonView::isoMapX)
			FirstPersonView *view = _isoView;
			const double toView = (double)_isoView->getWidth() / map->getWidth();
			map->setSelectorTransform([view, toView](int &x, int &y) {
				x = (int)std::floor(view->isoMapX((x + 0.5) * toView - 0.5));
				y = (int)std::floor(view->isoMapY((y + 0.5) * toView - 0.5));
			});
		}
		else map->setSelectorTransform(nullptr);
		// (the camera moving under a still mouse: the tile under it is another, see Map::setSelectorTransform)
		const double pose[6] = { _isoZoom, _isoShown[0] + _isoShot.pan[0], _isoShown[1] + _isoShot.pan[1], _isoTurnNow, _isoTiltNow, (double)map->getCamera()->getViewLevel() };
		if (pictureShown && !std::equal(pose, pose + 6, _isoLastPose))
		{
			map->refreshSelectorPosition();
			std::copy(pose, pose + 6, _isoLastPose);
		}
		// (where the player points on the map: the extra "downed icons" shows the body there, as 3D shows the one under the crosshair)
		int mouseX = 0, mouseY = 0;
		SDL_GetMouseState(&mouseX, &mouseY);
		Screen *screen = _game->getScreen();
		const double screenX = (mouseX - screen->getCursorLeftBlackBand()) / screen->getXScale(), screenY = (mouseY - screen->getCursorTopBlackBand()) / screen->getYScale();
		const double toView = (double)_isoView->getWidth() / map->getWidth();
		_isoView->setPointer((int)(screenX * toView), (int)(screenY * toView), 0); // (the map at the screen's corner: its pixels are the screen's)
	}
	bool dump = false;
	if (dumpFolder)
	{
		const std::string folder = dumpFolder;
		std::ifstream modeFile(folder + "/mode");
		int mode;
		if (modeFile >> mode) _isoMode = mode ? 1 : 0;
		std::ifstream goFile(folder + "/go");
		dump = goFile.good();
	}
	if (_isoFlight.on && _isoMode != 1)
	{
		// (the map switched to during a flight: the flight is over at once, into 3D as it would have ended, or back)
		const bool into3D = _isoFlight.to3D;
		BattleUnit *flown = _isoFlight.unit;
		endIsoFlight();
		if (into3D && flown)
		{
			_view->takeFlares(*_isoView); // (the flares burning go on in 3D)
			_trackedUnit = nullptr;
			_isoLandTick = SDL_GetTicks();
			setActive(true);
			return;
		}
	}
	if (_isoFlight.on && _isoMode == 1)
	{
		map->setOverlayTarget(nullptr);
		if (drawIsoFlight(SDL_GetTicks())) return;
		if (isActive()) return;
	}
	const bool show = _isoMode == 1 && !isActive() && !feedShown() && !map->showsHiddenMovement(); // (the map's HIDDEN MOVEMENT shows as it is)
	_isoView->setVisible(show);
	if (!show)
	{
		// (the map shows as it is: its cursor, clicks and scrolling its own)
		map->setSelectorTransform(nullptr);
		map->getCamera()->setScrollTransform(nullptr);
	}
	else if (_isoZoom != 1.0 || _isoTurnNow != 0.0 || _isoTiltNow != 0.0)
	{
		// Scrolling as the picture shows the map: as fast on the screen at any zoom, and the way the arrows point on a turned or
		// tilted picture (see isoMoveOnMap).
		const double turn = _isoTurnNow, tilt = _isoTiltNow, zoom = _isoZoom;
		map->getCamera()->setScrollTransform([turn, tilt, zoom](int &x, int &y) {
			double moveX = x, moveY = y;
			isoMoveOnMap(turn, tilt, zoom, moveX, moveY);
			x = (int)std::lround(moveX);
			y = (int)std::lround(moveY);
		});
	}
	else map->getCamera()->setScrollTransform(nullptr);
	_isoOverlay->setPalette(map->getPalette());
	// (the map's overlay pass, labelled for 3DIso, only while the picture shows on the map's own slant: turned or tilted it
	// isn't used, and the map's drawing costs half as much; back on the slant, drawn afresh at once)
	const bool overlayWanted = (show && _isoTurnNow == 0.0 && _isoTiltNow == 0.0) || dump;
	if (overlayWanted && !_isoOverlayWanted) map->invalidate();
	_isoOverlayWanted = overlayWanted;
	map->setOverlayTarget(overlayWanted ? _isoOverlay : nullptr);
	if (!show && !dump) return;
	Camera *camera = map->getCamera();
	_isoView->setPalette(map->getPalette());
	// (the map's levels: up to the view level; all of them with "show all layers"; the view level alone with "single layer")
	const int lowLevel = camera->getShowSingleLayer() ? camera->getViewLevel() : 0;
	const int highLevel = camera->getShowSingleLayer() ? camera->getViewLevel() : camera->getShowAllLayers() ? _save->getMapSizeZ() - 1 : camera->getViewLevel();
	_isoView->setNightVision(map->getNightVisionColor()); // (the map's night vision, as it is now)
	_isoView->setFlashlight(false, true);
	_isoView->setIsoVoxelShare(dump ? (_isoArt3D == 1 ? 1.0f : 0.0f) : (float)_isoVoxelShare);
	_isoView->setIsoCamera(camera->getMapOffset().x, camera->getMapOffset().y, camera->getViewLevel(), lowLevel, highLevel);
	{
		// Smooth level changes: the cut slides to the new level's top, in 3D and on the original art alike (the art below the
		// cut, see FirstPersonView::isoArtAlong).
		const double cutTarget = (highLevel + 1) * 24.0;
		if (!_isoCutSet || dump) _isoCutShown = cutTarget;
		_isoCutSet = true;
		// (eased, but at least IsoCutLeastSpeed voxels a second: it gets there, not only near it, so the floor of the level it
		// leaves, which shows as long as the cut is above it, goes on time; while it moves its edge is soft, so that floor
		// dissolves over the last stretch instead of going in one frame, see setIsoCut)
		const double IsoCutLeastSpeed = 30.0, IsoCutSoftness = 6.0;
		const double cutLeft = cutTarget - _isoCutShown;
		const double cutStep = std::max(std::abs(cutLeft) * std::min(1.0, _isoFrameSeconds * 7.0), IsoCutLeastSpeed * _isoFrameSeconds);
		_isoCutShown = std::abs(cutLeft) <= cutStep ? cutTarget : _isoCutShown + (cutLeft > 0.0 ? cutStep : -cutStep);
		_isoView->setIsoCut((float)_isoCutShown, _isoCutShown != cutTarget ? (float)IsoCutSoftness : 0.0f);
		// (leaving the map's slant on the original art: the camera moves at once, the last picture of the art dissolving into
		// the moving 3D one over IsoTurnMeltTime; coming back, the 3D fades into the art where it stands, see _isoVoxelShare)
		const double IsoTurnMeltTime = 220.0;
		const bool posed = _isoTurnNow != 0.0 || _isoTiltNow != 0.0;
		const Uint32 now = SDL_GetTicks();
		if (posed && !_isoWasPosed && _isoVoxelShare < 1.0 && !dump)
		{
			_isoView->keepPicture();
			_isoTurnMeltStart = now;
		}
		_isoWasPosed = posed;
		_isoView->setIsoDissolve(posed && now - _isoTurnMeltStart < IsoTurnMeltTime ? (float)((now - _isoTurnMeltStart) / IsoTurnMeltTime) : 1.0f);
	}
	_isoView->invalidate();
	if (dump)
	{
		const std::string folder = dumpFolder;
		map->invalidate();
		map->draw();
		_isoView->draw();
		SDL_SaveBMP(map->getSurface(), (folder + "/iso.bmp").c_str());
		SDL_SaveBMP(_isoView->getSurface(), (folder + "/iso3d.bmp").c_str());
		SDL_SaveBMP(_isoOverlay->getSurface(), (folder + "/overlay.bmp").c_str());
		{
			// (the depth of every pixel, as 32-bit floats row by row, for a picture of the 3D world behind it)
			std::ofstream depth(folder + "/depth.f32", std::ios::binary);
			depth.write((const char*)_isoView->isoDepths(), sizeof(float) * _isoView->getWidth() * _isoView->getHeight());
		}
		std::ofstream info(folder + "/info.txt");
		info << "offset " << camera->getMapOffset().x << " " << camera->getMapOffset().y << " level " << camera->getViewLevel()
			<< " size " << map->getWidth() << " " << map->getHeight() << "\n";
		std::remove((folder + "/go").c_str());
	}
}

}

namespace OpenXcom
{

/// The flight's length, and the share of it the map's picture takes to dissolve into the 3D one.
static const double IsoFlightDissolve = 0.2;
/// The isometric camera's distance (true proportions; see FirstPersonView::setIsoCamera).
static const double IsoFarBack = 524288.0;

void FirstPersonMode::toggleIsoArt()
{
	// A/R (the top bar's button, the key "3DIso: art"): turned, tilted or the free camera on, back to the map's own side, slant
	// and size, smoothly, onto the original art: a reset (the owner's word). On the map's own side and slant: our 3D art or the
	// original, the other one.
	const bool posed = _isoFree || std::abs(_isoTurnTarget - std::round(_isoTurnTarget / 360.0) * 360.0) > 1e-6 || std::abs(_isoTiltTarget) > 1e-6
		|| std::abs(_isoTurnNow - std::round(_isoTurnNow / 360.0) * 360.0) > 0.5 || std::abs(_isoTiltNow) > 0.5;
	if (posed)
	{
		setIsoFree(false);
		_isoZoomTarget = 1.0;
		_isoTurnTarget = std::round(_isoTurnNow / 360.0) * 360.0; // (the nearest whole turn: the short way round)
		_isoTiltTarget = 0.0;
		_isoArt3D = 0;
	}
	else _isoArt3D = _isoArt3D == 0 ? 1 : 0;
	// (remembered in the setting "3DIso art", unless a test asked for one)
	if (!getenv("OXC3D_ISO3D_ART"))
	{
		Options::fpsIsoArt = _isoArt3D == 1 ? 0 : 1;
		_isoArtSetting = Options::fpsIsoArt;
	}
}

/**
 * 3DIso's free camera on or off (research). On only where 3DIso turns (its extra "turn"); off, the view eases back to the map's
 * own side (the nearest whole turn, the short way), slant and size.
 */
void FirstPersonMode::setIsoFree(bool on)
{
	if (on && !(_isoExtrasOn && (_isoExtras & FirstPersonView::IsoExtraTurn))) on = false;
	if (on == _isoFree) return;
	_isoFree = on;
	_isoFreeDrag = IsoFreeDrag();
	std::fill(_isoFreeVelocity, _isoFreeVelocity + 4, 0.0);
	if (!on)
	{
		_isoZoomTarget = 1.0;
		_isoTurnTarget = std::round(_isoTurnNow / 360.0) * 360.0;
		_isoTiltTarget = 0.0;
	}
}

/**
 * 3DIso's zoom at a point of the map's picture (the game's screen pixels): the point stays where it is, the map's offset moving
 * by the change (in the picture's own terms: the camera's scroll transform makes it the map's, turned or not), the picture
 * gliding with it (see updateIsoView).
 */
void FirstPersonMode::isoZoomAt(double mouseX, double mouseY, double factor)
{
	if (_isoShot.phase == 1 || _isoShot.phase == 2) return; // (the shot camera has the zoom while it follows a shot)
	const double oldZoom = _isoZoomTarget, newZoom = std::clamp(oldZoom * factor, isoMinZoom(), 4.0);
	if (newZoom == oldZoom) return;
	Map *map = _state->_map;
	const double fromMiddleX = mouseX - map->getWidth() / 2.0, fromMiddleY = mouseY - map->getHeight() / 2.0;
	const double moveX = fromMiddleX * (1.0 / newZoom - 1.0 / oldZoom), moveY = fromMiddleY * (1.0 / newZoom - 1.0 / oldZoom);
	map->getCamera()->scrollXY((int)std::lround(moveX * _isoZoom), (int)std::lround(moveY * _isoZoom), true);
	_isoZoomTarget = newZoom;
}

/**
 * The free camera's mouse (research), over the map (not the top bar, the map's HUD or the map camera):
 * - the right button dragged turns the view round its middle (across) and tilts it (up: flatter, down: steeper);
 * - the middle button dragged moves it, the ground following the mouse;
 * - the wheel zooms at the mouse, from half the map's size to four times;
 * - a click that doesn't drag is the map's as ever (the right one turns the soldier or cancels), given to it on release;
 * - a left click on the marks turns the free camera off.
 * @return True if the event was the free camera's.
 */
bool FirstPersonMode::handleIsoFreeCamera(Action *action)
{
	const SDL_Event &event = *action->getDetails();
	const bool wheel = wheelStep(event) != 0; // (SDL 1.2's wheel buttons or SDL2's wheel)
	const bool isButton = !wheel && (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP);
	if (!wheel && !isButton && event.type != SDL_MOUSEMOTION) return false;
	if (wheel || isButton)
	{
		int eventX = 0, eventY = 0;
		mouseEventPosition(event, eventX, eventY);
		action->setMouseAction(eventX, eventY, 0, 0);
	}
	else action->setMouseAction(event.motion.x, event.motion.y, 0, 0);
	const double mouseX = action->getAbsoluteXMouse(), mouseY = action->getAbsoluteYMouse();
	const double canvasX = toCanvasX(mouseX), canvasY = toCanvasY(mouseY);
	if (isButton && event.button.button == SDL_BUTTON_LEFT && _isoFreeMarks->getVisible()
		&& canvasX >= _isoFreeMarks->getX() && canvasX < _isoFreeMarks->getX() + _isoFreeMarks->getWidth()
		&& canvasY >= _isoFreeMarks->getY() && canvasY < _isoFreeMarks->getY() + _isoFreeMarks->getHeight())
	{
		if (event.type == SDL_MOUSEBUTTONDOWN) setIsoFree(false);
		return true;
	}
	const bool overMap = canvasY >= BarY + BarH && mouseY < _state->_icons->getY() && !onMapCamera((int)canvasX, (int)canvasY);
	if (wheel)
	{
		if (!overMap) return false;
		isoZoomAt(mouseX, mouseY, wheelStep(event) < 0 ? 1.2 : 1.0 / 1.2); // (up: in)
		_isoFreeWheelTick = SDL_GetTicks();
		return true;
	}
	if (event.type == SDL_MOUSEBUTTONDOWN && (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) && !_isoFreeDrag.button)
	{
		if (!overMap) return false;
		_isoFreeDrag = IsoFreeDrag();
		_isoFreeDrag.button = event.button.button;
		_isoFreeDrag.startX = _isoFreeDrag.lastX = mouseX;
		_isoFreeDrag.startY = _isoFreeDrag.lastY = mouseY;
		mouseEventPosition(event, _isoFreeDrag.windowX, _isoFreeDrag.windowY);
		std::fill(_isoFreeVelocity, _isoFreeVelocity + 4, 0.0); // (a new grab stops a glide)
		return true;
	}
	if (event.type == SDL_MOUSEMOTION && _isoFreeDrag.button && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(_isoFreeDrag.button)))
	{
		_isoFreeDrag = IsoFreeDrag(); // (its release went elsewhere: another screen on top, the window left)
		return false;
	}
	if (event.type == SDL_MOUSEMOTION && _isoFreeDrag.button)
	{
		const double moveX = mouseX - _isoFreeDrag.lastX, moveY = mouseY - _isoFreeDrag.lastY;
		_isoFreeDrag.lastX = mouseX;
		_isoFreeDrag.lastY = mouseY;
		const double fromStartX = mouseX - _isoFreeDrag.startX, fromStartY = mouseY - _isoFreeDrag.startY;
		if (!_isoFreeDrag.moved && fromStartX * fromStartX + fromStartY * fromStartY < 9.0) return true; // (not yet a drag: maybe a click)
		_isoFreeDrag.moved = true;
		// (how fast it moves, smoothed over the last moves: what glides on when it's let go)
		const Uint32 now = SDL_GetTicks();
		const double step = std::clamp((now - _isoFreeMoveTick) / 1000.0, 0.004, 0.1);
		_isoFreeMoveTick = now;
		const double turnStep = -moveX * (1.0 / 3.0), tiltStep = moveY * 0.35;
		const bool turningDrag = _isoFreeDrag.button == SDL_BUTTON_RIGHT;
		const double speeds[4] = { turningDrag ? turnStep / step : 0.0, turningDrag ? tiltStep / step : 0.0, turningDrag ? 0.0 : moveX / step, turningDrag ? 0.0 : moveY / step };
		for (int axis = 0; axis < 4; ++axis) _isoFreeVelocity[axis] = _isoFreeVelocity[axis] * 0.5 + speeds[axis] * 0.5;
		// (the pointer back where the drag began as it nears the window's edge: a drag goes on as far as the player likes)
		{
			const int windowX = event.motion.x, windowY = event.motion.y;
			const int windowWidth = _game->getScreen()->getWidth(), windowHeight = _game->getScreen()->getHeight();
			if (windowX < windowWidth / 8 || windowX >= windowWidth * 7 / 8 || windowY < windowHeight / 8 || windowY >= windowHeight * 7 / 8)
			{
				SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
				SDL_WarpMouse((Uint16)_isoFreeDrag.windowX, (Uint16)_isoFreeDrag.windowY);
				SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
				_isoFreeDrag.lastX = _isoFreeDrag.startX;
				_isoFreeDrag.lastY = _isoFreeDrag.startY;
			}
		}
		if (turningDrag)
		{
			// (a third of a degree a pixel across; tilted between 10 and 80 degrees down)
			_isoTurnTarget += turnStep;
			_isoTiltTarget = std::clamp(_isoTiltTarget + tiltStep, -20.0, 50.0);
		}
		else
		{
			// (the move on the map's own picture, whole pixels to the map's camera, the rest kept for the next move: slow drags
			// at any zoom move it too)
			double moveOnMapX = moveX, moveOnMapY = moveY;
			isoMoveOnMap(_isoTurnNow, _isoTiltNow, _isoZoom, moveOnMapX, moveOnMapY);
			double *pan = _isoFreeDrag.pan;
			pan[0] += moveOnMapX;
			pan[1] += moveOnMapY;
			const int stepX = (int)pan[0], stepY = (int)pan[1];
			if (stepX || stepY) _state->_map->getCamera()->scrollXY(stepX, stepY, true, true);
			pan[0] -= stepX;
			pan[1] -= stepY;
		}
		return true;
	}
	if (event.type == SDL_MOUSEBUTTONUP && _isoFreeDrag.button && event.button.button == _isoFreeDrag.button)
	{
		const bool dragged = _isoFreeDrag.moved;
		_isoFreeDrag = IsoFreeDrag();
		if (!dragged) _state->mapClick(action); // (a click, not a drag: the map's, as ever)
		if (!dragged || SDL_GetTicks() - _isoFreeMoveTick > 60) std::fill(_isoFreeVelocity, _isoFreeVelocity + 4, 0.0); // (held still before it was let go: no glide)
		return true;
	}
	return false;
}

/**
 * The free camera's marks under the top bar's left, or under the map camera's window while it shows (research): what each
 * mouse control does, drawn smooth on 17 pixels (15 and an outline): a
 * four-way arrow (the middle button dragged moves the view), a turning arrow (the right button turns and tilts it), a
 * magnifier (the wheel zooms), each in the top bar's colour with soft edges and a dark outline, the one in use lit. Shown
 * while the free camera is on and 3DIso shows; a click on them turns it off.
 */
void FirstPersonMode::updateIsoFreeMarks()
{
	const bool show = _isoFree && iso3DOn() && _isoView->getVisible() && !_isoFlight.on && !isActive();
	_isoFreeMarks->setVisible(show);
	if (!show)
	{
		_isoFreeMarksLit = -1;
		_isoFreeDrag = IsoFreeDrag(); // (a drag ends with the picture it moved)
		return;
	}
	// (under the map camera's window while it shows: it would cover them, and take their clicks)
	const int marksY = _mapCameraFrame && _mapCameraFrame->getVisible() ? _mapCameraFrame->getY() + _mapCameraFrame->getHeight() + 2 : BarY + BarH + 2;
	if (_isoFreeMarks->getY() != marksY) _isoFreeMarks->setY(marksY);
	const bool wheeled = SDL_GetTicks() - _isoFreeWheelTick < 300;
	const int lit = _isoFreeDrag.moved ? (_isoFreeDrag.button == SDL_BUTTON_MIDDLE ? 1 : 2) : wheeled ? 3 : 0;
	if (lit == _isoFreeMarksLit) return;
	_isoFreeMarksLit = lit;
	// Each mark as a shape on its own 15 x 15 square (its middle at 7.5, 7.5) a pixel in from its cell's edge (room for the
	// outline), sampled 4 x 4 a pixel for its share of each pixel.
	auto inTriangle = [](double x, double y, double ax, double ay, double bx, double by, double cx, double cy) {
		const double first = (bx - ax) * (y - ay) - (by - ay) * (x - ax), second = (cx - bx) * (y - by) - (cy - by) * (x - bx), third = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
		return (first >= 0 && second >= 0 && third >= 0) || (first <= 0 && second <= 0 && third <= 0);
	};
	auto nearSegment = [](double x, double y, double ax, double ay, double bx, double by, double half) {
		const double dx = bx - ax, dy = by - ay, along = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
		const double ox = ax + dx * along - x, oy = ay + dy * along - y;
		return ox * ox + oy * oy <= half * half;
	};
	auto inside = [&](int mark, double x, double y) {
		const double cx = 7.5, cy = 7.5, dx = x - cx, dy = y - cy;
		if (mark == 0)
		{
			// (moving: a cross with an arrowhead at each end)
			if (std::abs(dy) <= 0.9 && std::abs(dx) <= 5.0) return true;
			if (std::abs(dx) <= 0.9 && std::abs(dy) <= 5.0) return true;
			for (int side = 0; side < 4; ++side)
			{
				const double ux = side == 0 ? 1 : side == 1 ? -1 : 0, uy = side == 2 ? 1 : side == 3 ? -1 : 0;
				if (inTriangle(dx, dy, ux * 7.4, uy * 7.4, ux * 4.2 - uy * 3.0, uy * 4.2 + ux * 3.0, ux * 4.2 + uy * 3.0, uy * 4.2 - ux * 3.0)) return true;
			}
			return false;
		}
		if (mark == 1)
		{
			// (turning: a ring open at the top right, an arrowhead at its upper end going round)
			const double radius = std::sqrt(dx * dx + dy * dy), angle = std::atan2(-dy, dx) * 180.0 / Pi; // (0 right, 90 up)
			if (std::abs(radius - 5.0) <= 0.95 && !(angle > 10.0 && angle < 80.0)) return true;
			return inTriangle(dx, dy, 1.0, -2.4, 1.0, -7.8, 5.4, -5.1); // (at the ring's top, pointing right: round clockwise)
		}
		// (zooming: a magnifier, its glass up left, its handle down right)
		const double gx = x - 6.0, gy = y - 6.0, radius = std::sqrt(gx * gx + gy * gy);
		if (std::abs(radius - 4.0) <= 1.0) return true;
		return nearSegment(x, y, 9.2, 9.2, 13.4, 13.4, 1.35);
	};
	const SDL_Color *palette = _state->getPalette();
	const Uint8 base = _btnIsoArt->getColor();
	// (the bar's colour; lit: the palette's nearest to a pale gold; edges a shade darker in the colour's own group)
	const Uint8 fill = base, edge = (Uint8)std::min(255, (int)base + 2), bright = nearestColor(palette, 255, 240, 150), brightEdge = nearestColor(palette, 220, 190, 90),
		outline = nearestColor(palette, 8, 8, 8);
	_isoFreeMarks->clear();
	_isoFreeMarks->lock();
	const int Cell = IsoFreeMarkSize;
	for (int mark = 0; mark < 3; ++mark)
	{
		const int left = mark * (Cell + 2);
		double share[Cell][Cell];
		for (int y = 0; y < Cell; ++y)
			for (int x = 0; x < Cell; ++x)
			{
				int covered = 0;
				for (int sub = 0; sub < 16; ++sub) covered += inside(mark, x - 1 + (sub % 4 + 0.5) / 4.0, y - 1 + (sub / 4 + 0.5) / 4.0) ? 1 : 0;
				share[y][x] = covered / 16.0;
			}
		const bool on = lit == mark + 1;
		for (int y = 0; y < Cell; ++y)
			for (int x = 0; x < Cell; ++x)
			{
				if (share[y][x] >= 0.55) { _isoFreeMarks->setPixel(left + x, y, on ? bright : fill); continue; }
				if (share[y][x] >= 0.2) { _isoFreeMarks->setPixel(left + x, y, on ? brightEdge : edge); continue; }
				bool touches = false;
				for (int dy = -1; dy <= 1 && !touches; ++dy)
					for (int dx = -1; dx <= 1 && !touches; ++dx)
					{
						const int nx = x + dx, ny = y + dy;
						touches = nx >= 0 && ny >= 0 && nx < Cell && ny < Cell && share[ny][nx] >= 0.2;
					}
				if (touches) _isoFreeMarks->setPixel(left + x, y, outline);
			}
	}
	_isoFreeMarks->unlock();
}

/**
 * 3DIso turned or tilted (research): the 3D view's compass on the map (setting "Compass"), its bearing 3DIso's camera's, the
 * selected soldier's facing on its rim: which way north is once the map isn't seen from its own side.
 */
void FirstPersonMode::updateIsoCompass()
{
	const bool show = Options::fpsCompass && iso3DOn() && _isoView->getVisible() && !_isoFlight.on && (_isoTurnNow != 0.0 || _isoTiltNow != 0.0) && !hudHidden();
	if (!show) return; // (hidden by updateCompass already)
	_compass->setVisible(true);
	for (Text *letter : _compassLetters) letter->setVisible(true);
	const int viewBearing = ((int)std::lround(-135.0 + _isoTurnNow + 90.0) % 360 + 360) % 360;
	const BattleUnit *unit = _save->getSelectedUnit();
	const int facing = unit ? unit->getDirection() : ((viewBearing + 22) / 45) % 8;
	if (viewBearing == _compassDrawn[0] && facing == _compassDrawn[1] && _compassDrawn[2] == 0 && _compassDrawn[3] == -1) return;
	_compassDrawn[0] = viewBearing;
	_compassDrawn[1] = facing;
	_compassDrawn[2] = 0;
	_compassDrawn[3] = -1;
	_compassDrawn[4] = _compassDrawn[5] = -1; // (3D draws its own again)
	drawCompass(viewBearing, facing, 0, -1);
	const double radius = CompassSize / 2 - 9.0;
	for (int letter = 0; letter < 4; ++letter)
	{
		const double angle = (letter * 90 - viewBearing) * Pi / 180.0;
		_compassLetters[letter]->setX((int)std::lround(_compass->getX() + CompassSize / 2.0 + radius * std::sin(angle) - 3.5));
		_compassLetters[letter]->setY((int)std::lround(_compass->getY() + CompassArrowH + CompassSize / 2.0 - radius * std::cos(angle) - 3.5));
	}
}

void FirstPersonMode::endIsoFlight()
{
	_isoFlight.on = false;
	_isoHudSlide = 0.0; // (the map's HUD back in its place)
	if (_isoView)
	{
		_isoView->setFlightCeiling(0.0f);
		_isoView->setFlightUnknownDark(0.0f);
		_isoView->setFlightDistanceShare(1.0f);
		_isoView->endFlight();
	}
}

bool FirstPersonMode::isoShowsEffects() const
{
	return _isoView && _isoView->getVisible() && !isActive() && (_isoView->isoExtras() & FirstPersonView::IsoExtraEffects);
}

bool FirstPersonMode::iso3DOn() const
{
	return (getenv("OXC3D_ISO3D") || Options::fpsIso) && _isoMode == 1 && _isoView;
}

/**
 * Starts the flight (see IsoFlight): into the soldier's eyes (the 3D view's camera as 3D starts: the middle of their tile at
 * eye height, the way they face, the field of view now) or out of them (the caller sets where the 3D view looked). The
 * map's picture at its end is drawn now and kept, to dissolve from or into.
 */
void FirstPersonMode::startIsoFlight(BattleUnit *unit, bool to3D)
{
	// (into the eyes from wherever 3DIso is turned, tilted and zoomed, and back onto the same: the camera is kept while in 3D,
	// the free camera too (the owner's word); a drag and a glide end)
	_isoFreeDrag = IsoFreeDrag();
	std::fill(_isoFreeVelocity, _isoFreeVelocity + 4, 0.0);
	_isoTurnNow = _isoTurnTarget;
	_isoTiltNow = _isoTiltTarget;
	_isoZoom = _isoZoomTarget;
	if (!to3D) _isoView->takeFlares(*_view); // (the flares burning in 3D go on in 3DIso)
	_isoFlight.startYaw = -135.0 + _isoTurnNow;
	_isoFlight.startPitch = std::clamp(-30.0 - _isoTiltNow, -80.0, -10.0);
	_isoFlight.startBack = _isoView->isoBack();
	_isoFlight.on = true;
	_isoFlight.to3D = to3D;
	_isoFlight.start = SDL_GetTicks();
	_isoFlight.unit = unit;
	const Position position = unit->getPosition();
	const int unitSize = unit->getArmor()->getSize();
	_isoFlight.eye[0] = position.x * 16.0 + 8.0 * unitSize;
	_isoFlight.eye[1] = position.y * 16.0 + 8.0 * unitSize;
	_isoFlight.eye[2] = eyeHeight(unit);
	_isoFlight.yaw = unit->getDirection() * 45.0 - 90.0;
	_isoFlight.pitch = 0.0;
	_isoFlight.fov = fieldOfViewNow();
	// The map's picture of its end: 3DIso drawn now (with the map's overlays as they are now) and kept. Only on the original
	// art, which the flight dissolves from or into: our 3D art is the flight's own picture (see drawIsoFlight), and drawing
	// it here made the flight's first frame two frames long.
	if (_isoArt3D == 1)
	{
		return;
	}
	Map *map = _state->_map;
	Camera *camera = map->getCamera();
	map->setOverlayTarget(_isoOverlay);
	map->invalidate();
	map->draw();
	const int lowLevel = camera->getShowSingleLayer() ? camera->getViewLevel() : 0;
	const int highLevel = camera->getShowSingleLayer() ? camera->getViewLevel() : camera->getShowAllLayers() ? _save->getMapSizeZ() - 1 : camera->getViewLevel();
	_isoView->setPalette(map->getPalette());
	_isoView->setIsoCamera(camera->getMapOffset().x, camera->getMapOffset().y, camera->getViewLevel(), lowLevel, highLevel);
	_isoView->invalidate();
	_isoView->draw();
	_isoView->keepPicture();
}

/**
 * A frame of the flight, in two stretches (s: 0 the map, 1 the soldier's eyes).
 * - Up to IsoFlightOver: the camera turns about a pivot (true proportions), from the middle of the map's picture seen from
 *   IsoFarBack along the isometric slant (the map's sqrt(2) pixels to a voxel there) to the soldier's head seen from
 *   IsoFlightAbove away, steeply from above (IsoFlightDivePitch) and the way they face, with the 3D field of view. The
 *   pivot moves straight, the angles turn the short way, and the distance and the width seen at the pivot change by the
 *   same factor each moment (a dolly zoom: the parallel picture grows its perspective smoothly).
 * - Then the dive: from there straight into the soldier's eyes, the view coming up level.
 * The levels above the map's view level stay away (the map's cut, moved to the soldier's level: what's over them hides
 * nothing on the way down, and inside their level the rays start at the eye); the map's picture dissolves over the first
 * IsoFlightDissolve; the soldier is looked through (not drawn) once the eye is close to their head.
 * @return False once it's over (into 3D: the 3D view takes over in this very pose).
 */
bool FirstPersonMode::drawIsoFlight(Uint32 now)
{
	const double ZScaleTrue = 0.81649658092772603, Degree = 3.14159265358979323846 / 180.0;
	const double IsoFlightOver = 0.65, IsoFlightAbove = 56.0, IsoFlightDivePitch = -70.0;
	// (OXC3D_ISO3D_FLIGHT_HOLD=s: a test's, the flight held at that point for ever: 0 the map, 1 the soldier's eyes)
	static const char *hold = getenv("OXC3D_ISO3D_FLIGHT_HOLD");
	const double t = hold && *hold ? 0.5 : std::clamp((now - _isoFlight.start) / (isoFlightSeconds() * 1000.0), 0.0, 1.0);
	const double eased = t * t * (3.0 - 2.0 * t);
	const double s = hold && *hold ? std::atof(hold) : _isoFlight.to3D ? eased : 1.0 - eased;
	Map *map = _state->_map;
	Camera *camera = map->getCamera();
	auto forwardOf = [&](double yaw, double pitch, double out[3]) {
		out[0] = std::cos(yaw * Degree) * std::cos(pitch * Degree);
		out[1] = std::sin(yaw * Degree) * std::cos(pitch * Degree);
		out[2] = std::sin(pitch * Degree);
	};
	auto smooth = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); };
	// The ends and the point above the head between the stretches (true proportions).
	double isoPivot[3];
	_isoView->isoPivot(camera->getMapOffset().x, camera->getMapOffset().y, camera->getViewLevel(), isoPivot);
	isoPivot[2] *= ZScaleTrue;
	const double head[3] = { _isoFlight.eye[0], _isoFlight.eye[1], _isoFlight.eye[2] * ZScaleTrue };
	const double isoWidth = _isoView->getWidth() / 2.0 / std::sqrt(2.0) / std::max(1e-6, _isoView->isoZoom()); // (the width seen at the pivot, zoomed: see setIsoScale)
	const double halfFov = std::tan(_isoFlight.fov * Degree / 2.0);
	double eye[3], yaw, pitch, halfFovTangent;
	if (s < IsoFlightOver)
	{
		const double u = smooth(s / IsoFlightOver);
		double pivot[3];
		for (int axis = 0; axis < 3; ++axis) pivot[axis] = isoPivot[axis] + (head[axis] - isoPivot[axis]) * u;
		yaw = _isoFlight.startYaw + std::remainder(_isoFlight.yaw - _isoFlight.startYaw, 360.0) * u;
		pitch = _isoFlight.startPitch + (IsoFlightDivePitch - _isoFlight.startPitch) * u;
		const double farBack = _isoFlight.startBack; // (where 3DIso's eye stood: far back, nearer tilted flat)
		const double distance = std::exp(std::log(farBack) + (std::log(IsoFlightAbove) - std::log(farBack)) * u);
		const double width = std::exp(std::log(isoWidth) + (std::log(IsoFlightAbove * halfFov) - std::log(isoWidth)) * u);
		double forward[3];
		forwardOf(yaw, pitch, forward);
		for (int axis = 0; axis < 3; ++axis) eye[axis] = pivot[axis] - forward[axis] * distance;
		halfFovTangent = width / distance;
	}
	else
	{
		const double u = smooth((s - IsoFlightOver) / (1.0 - IsoFlightOver));
		double forward[3];
		forwardOf(_isoFlight.yaw, IsoFlightDivePitch, forward);
		for (int axis = 0; axis < 3; ++axis) eye[axis] = head[axis] - forward[axis] * IsoFlightAbove * (1.0 - u);
		yaw = _isoFlight.yaw;
		pitch = IsoFlightDivePitch + (_isoFlight.pitch - IsoFlightDivePitch) * u;
		halfFovTangent = halfFov;
	}
	const double headDistance = std::sqrt((eye[0] - head[0]) * (eye[0] - head[0]) + (eye[1] - head[1]) * (eye[1] - head[1]) + (eye[2] - head[2]) * (eye[2] - head[2]));
	eye[2] /= ZScaleTrue;
	// The cut: from the map's (its view level's top, or the map's top with all layers shown) to the soldier's level's top.
	const int highLevel = camera->getShowAllLayers() ? _save->getMapSizeZ() - 1 : camera->getViewLevel();
	const float isoTop = (float)((highLevel + 1) * Position::TileZ), unitTop = (float)((_isoFlight.unit->getPosition().z + 1) * Position::TileZ);
	const float cutTop = isoTop + (unitTop - isoTop) * (float)smooth(s / 0.3);
	_isoView->setSmoothLighting(Options::fpsSmoothLighting);
	_isoView->setMaxDistance(FirstPerson::viewDistance(_game->getMod()));
	_isoView->setNightVision(map->getNightVisionColor());
	_isoView->setFlashlight(_flashlightOn, true);
	// (our 3D art: the flight's first picture is 3DIso's own, no dissolve; the original art dissolves into it)
	const float shown3D = _isoArt3D == 1 ? 1.0f : (float)std::clamp(s / IsoFlightDissolve, 0.0, 1.0);
	_isoView->setFlightCamera(eye, yaw, pitch, halfFovTangent, cutTop, headDistance < 20.0 ? _isoFlight.unit : nullptr, shown3D);
	// (the ceiling over the soldier fading in through the dive's last stretch, as the 3D view will show it: no pop at the end)
	_isoView->setFlightCeiling((float)smooth((s - 0.82) / 0.18));
	// (what the squad hasn't found: black as 3DIso and the map have it, dissolving into the 3D view's way over the first stretch)
	_isoView->setFlightUnknownDark((float)(1.0 - smooth(s / 0.4)));
	// (the 3D view's darkening and haze with distance: none on the map, coming in over the first stretch)
	_isoView->setFlightDistanceShare((float)smooth(s / 0.6));
	// (the map's HUD sliding away in the flight's first stretch into the eyes, back in its last stretch out of them)
	_isoHudSlide = smooth(s / 0.25);
	_isoView->setVisible(true);
	_isoView->invalidate();
	// (OXC3D_ISO3D_FLIGHTDUMP: each frame of the flight saved as a picture, numbered)
	static const char *flightDump = getenv("OXC3D_ISO3D_FLIGHTDUMP");
	if (flightDump && *flightDump)
	{
		static int frame = 0;
		_isoView->draw();
		char name[64];
		snprintf(name, sizeof(name), "/flight%04d.bmp", frame++);
		SDL_SaveBMP(_isoView->getSurface(), (std::string(flightDump) + name).c_str());
	}
	if (t < 1.0) return true;
	// Over: into 3D (the 3D view in this pose), or back on 3DIso.
	endIsoFlight();
	if (_isoFlight.to3D)
	{
		_isoView->setVisible(false);
		_view->takeFlares(*_isoView); // (the flares burning go on in 3D)
		_trackedUnit = nullptr; // (the 3D view starts as the flight ended: the soldier's eyes, the way they face)
		_isoFlightLanding = true; // (exactly the flight's last pose: see updateCamera)
		_isoLandTick = SDL_GetTicks(); // (the weapon comes up from below, not there at once)
		_isoFlightLandYaw = _isoFlight.yaw;
		_isoFlightLandPitch = _isoFlight.pitch;
		setActive(true);
	}
	return false;
}

}

namespace OpenXcom
{

/**
 * 3DIso's tactical flare: from the selected soldier's right shoulder (their eyes' height, a little to the side of the throw)
 * on the arc that comes down on the tile the map's cursor is on (its floor's middle), as the flare key throws one in 3D at the
 * crosshair; with the game's throwing sound from the soldier's tile, and the strike and hiss.
 */
void FirstPersonMode::throwIsoFlare()
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !unit->getTile() || unit->getFaction() != FACTION_PLAYER || _save->getSide() != FACTION_PLAYER) return;
	Position target;
	_state->_map->getSelectorPosition(&target);
	const Tile *targetTile = _save->getTile(target);
	if (!targetTile) return;
	const Position position = unit->getPosition();
	const int unitSize = unit->getArmor()->getSize();
	const float aim[3] = { target.x * 16.0f + 8.0f, target.y * 16.0f + 8.0f, target.z * 24.0f + 2.0f - targetTile->getTerrainLevel() };
	float from[3] = { position.x * 16.0f + 8.0f * unitSize, position.y * 16.0f + 8.0f * unitSize, (float)eyeHeight(unit) - 2.0f };
	const float towardX = aim[0] - from[0], towardY = aim[1] - from[1], flat = std::max(1.0f, std::sqrt(towardX * towardX + towardY * towardY));
	from[0] += towardX / flat * 3.0f - towardY / flat * 3.0f; // (ahead and to the right, as from the shoulder in 3D)
	from[1] += towardY / flat * 3.0f + towardX / flat * 3.0f;
	_isoView->throwFlareFrom(from, aim);
	if (Sound *sound = _game->getMod()->getSoundByDepth(_save->getDepth(), Mod::ITEM_THROW))
	{
		sound->play(-1, _state->_map->getSoundAngle(position));
	}
	_sounds->play(FirstPersonSounds::FLARE, 0, 0.55f);
}

}
