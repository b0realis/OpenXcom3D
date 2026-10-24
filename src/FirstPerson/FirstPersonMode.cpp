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
#include <climits>
#include <cmath>
#include <string>
#include "FirstPersonApi.h"
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/ExplosionBState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/Timer.h"
#include "../Interface/Bar.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/SavedGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

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
/// Whether the 3D HUD was hidden (FirstPersonMode::_hudHidden).
bool lastHudHidden = false;
/// Whether the flashlight was switched on (FirstPersonMode::_flashlightOn).
bool lastFlashlightOn = true;
/// Whether sprinting and en passant were switched on (FirstPersonMode::_sprintOn, _enPassantOn).
bool lastSprintOn = false, lastEnPassantOn = false;
/// The cheats the doom mode key would switch back on (FirstPersonMode::_pausedCheats).
int lastPausedCheats = 0;
/// Whether the akimbo key would switch realistic akimbo back on (FirstPersonMode::_akimboRealisticWasOn).
bool lastAkimboRealisticWasOn = true;
/// The Doom real-time fuses burning (FirstPersonMode::_doomFuses): only for the same battle.
std::unordered_map<int, FirstPersonMode::DoomFuse> lastDoomFuses;
/// When a first-person mode last ran its per-frame update (SDL ticks, ms; 0: never).
Uint32 lastThinkTicks = 0;
/// The map camera's size the 3D HUD key brings back on the map (see toggleMapCamera): the last
/// one it had, medium at first.
int lastMapCameraSize = 2;

/// The shoulder flashlight (setting "Flashlight": On) lights from the battle's shade 6 on (of 0
/// bright .. 15 dark): at dusk and night.
const int FlashlightDarkShade = 6;

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
	_game(game), _state(state), _save(state->_save), _wants3D(Options::fpsStartInFirstPerson), _missionAtStart(state->_save->getMissionType()), _warningY(state->_warning->getY())
{
	// The overlays are laid out on a canvas; on big screens it's drawn magnified (see blit).
	// Whether each step is allowed is not known yet (see stepAllowed).
	_uiScale = uiScaleFor(Options::baseXResolution, Options::baseYResolution);
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
	FirstPerson::settleOptions(); // (the smoke's first value, from an older setting)

	// Our surfaces, in drawing order: each one goes on top of the ones made before it.
	createView();
	createCrosshairAndBanner();
	createLiftArrows();
	createCompass();
	createWalkModeLines();
	createTopBar();
	createInfoPanel();
	makeMapCamera(Options::fpsMapCamera > 0 ? Options::fpsMapCamera : lastMapCameraSize); // (under the cards)
	createSoldierCard();
	createSoldierCam(); // (last: drawn over everything)

	// The mode's own sounds, and the canvas the overlays are drawn on before being magnified.
	_sounds = new FirstPersonSounds();
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
		_flashlightOn = lastFlashlightOn;
		_sprintOn = lastSprintOn;
		_enPassantOn = lastEnPassantOn;
		_pausedCheats = lastPausedCheats;
		_akimboRealisticWasOn = lastAkimboRealisticWasOn;
		if (lastBattle == _save) _doomFuses = lastDoomFuses;
		_mouseLook = !_classicHud;
	}
	rememberChoices();
}

/**
 * Makes the 3D view: full screen, inserted right after the isometric map it replaces (so the
 * classic HUD still draws over it). It renders at the canvas size and is magnified with the
 * overlays, so a big screen costs what 640x400 does. It stays hidden until the player
 * switches to 3D (see setActive).
 */
void FirstPersonMode::createView()
{
	const int viewWidth = canvasWidth();
	const int viewHeight = canvasHeight();
	const Mod *mod = _game->getMod();

	// The state adds a surface at the end (on top of everything): move it to right after the
	// isometric map instead.
	_view = new FirstPersonView(_game, viewWidth, viewHeight, 0, 0);
	_state->add(_view);
	auto &surfaces = _state->_surfaces;
	surfaces.pop_back();
	surfaces.insert(std::find(surfaces.begin(), surfaces.end(), _state->_map) + 1, _view);

	// What it shows: this battle, with the isometric map's shots and explosions, as far as the
	// mod allows.
	_view->setSavedBattleGame(_save);
	_view->setMap(_state->_map);
	_view->setMaxDistance(mod->getFirstPerson().getMaxViewDistance());
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
	_txtMode = addText(120, 9, width / 2 + 14, height / 2 - 9, "crosshairText", "firstPerson");
	_txtShot = addText(120, 9, width / 2 + 14, height / 2 + 1, "crosshairText", "firstPerson");
	_txtBanner->setAlign(ALIGN_CENTER);
	// The name of the unit pointed at, under the crosshair (it follows the free cursor).
	_txtTarget = addText(160, 9, width / 2 - 80, height / 2 + 10, "crosshairText", "firstPerson");
	_txtTarget->setAlign(ALIGN_CENTER);
	// Zoomed in: SCOPING and the zoom, at the right just under the scope cross's line (clear of
	// the lift arrows' texts above it).
	_txtScoping = addText(90, 9, width - 94, height / 2 + 5, "crosshairText", "firstPerson");
	_txtScoping->setAlign(ALIGN_RIGHT);

	// All hidden until there's something to show.
	for (Text *text : { _txtMode, _txtShot, _txtBanner, _txtTarget, _txtScoping })
	{
		text->setVisible(false);
	}
	_crosshair->setVisible(false);
	_ownSurfaces.insert(_crosshair);
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
 * Makes the compass (setting "Compass"): a round disc at the top right, under the top bar, with
 * the heading arrow on top of it, the four letters of the compass going round it, and a line under
 * it that warns when looking further will turn the soldier at a cost (see updateCompass). All
 * hidden until the 3D view shows.
 */
void FirstPersonMode::createCompass()
{
	const int width = canvasWidth(); // our surfaces are laid out on the canvas (see makeCanvas)
	const int compassX = width - CompassSize - 4, compassY = BarY + BarH + CompassGap;
	_compass = new Surface(CompassSize, CompassArrowH + CompassSize, compassX, compassY);
	_state->add(_compass);
	_compass->setVisible(false);
	_ownSurfaces.insert(_compass);

	// The letters, small, north in red (as on a compass rose), the others in the buttons' grey.
	const char *const letters[4] = { "N", "E", "S", "W" };
	for (int letter = 0; letter < 4; ++letter)
	{
		_compassLetters[letter] = addText(7, 8, compassX, compassY, "button", "firstPerson");
		_compassLetters[letter]->setSmall();
		_compassLetters[letter]->setAlign(ALIGN_CENTER);
		_compassLetters[letter]->setText(letters[letter]);
		_compassLetters[letter]->setColor(letter == 0 ? textColor(_state->getPalette(), 230, 60, 50) : textColor(_state->getPalette(), 200, 200, 210));
		_compassLetters[letter]->setVisible(false);
	}

	// The warning line under the dial, ending at its right edge (the dial sits against the
	// screen's right edge, and a line centred under it would run off it).
	_txtCompassTurn = addText(CompassSize + 40, 9, compassX - 40, compassY + CompassArrowH + CompassSize + 1, "weapon", "firstPerson");
	_txtCompassTurn->setAlign(ALIGN_RIGHT);
	_txtCompassTurn->setVisible(false);
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
	_btnPrev = makeButton(SquadX, PrevNextW, "<");
	_btnNext = makeButton(SquadX, PrevNextW, ">");

	// The suit's buttons, shown for what it can do (laid out right to left from END): fly (swim
	// under the sea), personal light, night vision and a special weapon. Each label comes in
	// full and short, for a narrow bar; the buttons start as wide as the full label.
	const bool underwater = _save->getDepth() > 0;
	const char *suitLabels[4][2] = {
		{ underwater ? "STR_FPS_BUTTON_SWIM" : "STR_FPS_BUTTON_FLY", underwater ? "STR_FPS_BUTTON_SWIM_SHORT" : "STR_FPS_BUTTON_FLY_SHORT" },
		{ "STR_FPS_BUTTON_LIGHT", "STR_FPS_BUTTON_LIGHT_SHORT" }, { "STR_FPS_BUTTON_NIGHT", "STR_FPS_BUTTON_NIGHT_SHORT" },
		{ "STR_FPS_BUTTON_SPECIAL", "STR_FPS_BUTTON_SPECIAL_SHORT" } };
	TextButton **suitButtons[4] = { &_btnFly, &_btnLight, &_btnNight, &_btnSpecial };
	for (int buttonIndex = 0; buttonIndex < 4; ++buttonIndex)
	{
		_suitLabelsLong[buttonIndex] = _state->tr(suitLabels[buttonIndex][0]);
		_suitLabelsShort[buttonIndex] = _state->tr(suitLabels[buttonIndex][1]);
		*suitButtons[buttonIndex] = makeButton(0, labelWidth(_suitLabelsLong[buttonIndex]), _suitLabelsLong[buttonIndex]);
		(*suitButtons[buttonIndex])->setVisible(false);
	}

	// Flight, light and night vision are toggles: each a button group of its own, so the
	// button stays pressed while it's on.
	_btnFly->setGroup(&_flyButtonGroup);
	_btnLight->setGroup(&_lightButtonGroup);
	_btnNight->setGroup(&_nightButtonGroup);
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
	_txtName = addText(PanelW - 66, 9, panelX + 3, panelY + 2, "textName", "battlescape");
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

	// Last line: the armor, and DOOM on the right.
	_txtArmor = addText(PanelLineW, 9, panelX + 3, panelY + 44, "weapon", "firstPerson");
	_txtDoom = addText(60, 9, panelX + PanelW - 63, panelY + 44, "doom", "firstPerson");

	// The panel's parts, all ours and all hidden until the 3D view shows.
	_panelParts = { _txtName, _txtStatus, _txtWeapon, _txtWeaponAmmo, _txtOtherWeapon, _txtOtherAmmo, _numTimeUnits, _barTimeUnits,
		_numEnergy, _barEnergy, _numHealth, _barHealth, _numMorale, _barMorale, _txtArmor, _txtDoom };
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
	for (auto *bar : { _barTimeUnits, _barEnergy, _barHealth, _barMorale })
	{
		bar->setScale(1.0);
	}
	// The stats' own colours, for when Doom IDDQD has turned them silver and stops.
	const NumberText *numbers[4] = { _numTimeUnits, _numEnergy, _numHealth, _numMorale };
	const Bar *bars[4] = { _barTimeUnits, _barEnergy, _barHealth, _barMorale };
	for (int stat = 0; stat < 4; ++stat)
	{
		_statColors[stat] = numbers[stat]->getColor();
		_statColors[4 + stat] = bars[stat]->getColor();
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
 * Makes the soldier cam, hidden: a soldier's card's helmet camera enlarged, with a right click
 * on their squad button (see updateSoldierCam). A frame with a title bar ("SOLDIER CAM: name",
 * a blinking red dot, as a live feed has) round a second small 3D view, with a little TV
 * static over it, so it's plain that it's a camera, not the view the player plays in. The
 * picture and the frame are made at the canvas's size (see makeSoldierCamView).
 */
void FirstPersonMode::createSoldierCam()
{
	makeSoldierCamView();

	// The red dot: a filled circle 5 pixels across.
	_soldierCamDot = new Surface(5, 5, 0, 0);
	_state->add(_soldierCamDot);
	_soldierCamDot->setPalette(_state->getPalette());
	const Uint8 red = nearestColor(_state->getPalette(), 230, 30, 30);
	_soldierCamDot->lock();
	for (int dotY = 0; dotY < 5; ++dotY)
	{
		for (int dotX = 0; dotX < 5; ++dotX)
		{
			if ((dotX - 2) * (dotX - 2) + (dotY - 2) * (dotY - 2) <= 5) _soldierCamDot->setPixel(dotX, dotY, red);
		}
	}
	_soldierCamDot->unlock();

	// The title.
	_txtSoldierCam = new Text(220, 9, 0, 0);
	_state->add(_txtSoldierCam, "textName", "battlescape");
	_txtSoldierCam->setHighContrast(true);

	for (Surface *part : { (Surface*)_soldierCamDot, (Surface*)_txtSoldierCam })
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
	camera->setSavedBattleGame(_save);
	camera->setMap(_state->_map);
	camera->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	camera->setParticles(false);
	const int signalLevel = mod->getFirstPerson().getSignalLevel(_save->getMissionType());
	camera->setStatic(signalLevel >= 2 ? 0.05f : 0.03f); // a little TV static: a camera's picture (more on the hardest missions)
	camera->setSignalGlitches(signalLevel); // and a digital feed's bad signal now and then (a lot there)

	// In place of the old ones (or, the first time, added last: drawn over everything).
	putSurfaceInPlace(_soldierCamFrame, frame);
	putSurfaceInPlace(_soldierCam, camera);
	_soldierCamFrame = frame;
	_soldierCam = camera;
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
		std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), oldSurface, newSurface);
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
	camera->setSavedBattleGame(_save);
	camera->setMap(_state->_map);
	camera->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	camera->setParticles(false);
	camera->setSignalGlitches(mod->getFirstPerson().getSignalLevel(_save->getMissionType()));

	putSurfaceInPlace(_mapCameraFrame, frame);
	putSurfaceInPlace(_mapCamera, camera);
	_mapCameraFrame = frame;
	_mapCamera = camera;

	// The title (made the first time), as wide as the picture.
	if (!_txtMapCamera)
	{
		_txtMapCamera = new Text(pictureWidth, 9, 0, 0);
		_state->add(_txtMapCamera, "textName", "battlescape");
		_txtMapCamera->setHighContrast(true);
		_txtMapCamera->setVisible(false);
		_ownSurfaces.insert(_txtMapCamera);
	}
	_txtMapCamera->setWidth(pictureWidth - 2);
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
	for (Surface *part : std::initializer_list<Surface*>{ _liftUp, _liftDown, _txtLift, _txtLiftKeys }) _anchors[part] = RIGHT_CENTER;
	_anchors[_txtMode] = CENTER;
	_anchors[_txtShot] = CENTER;
	_anchors[_txtScoping] = RIGHT_CENTER;

	// END and the suit's buttons keep to the top right, and so does the compass; the info panel
	// to the bottom left.
	_anchors[_btnEndTurn] = TOP_RIGHT;
	_anchors[_compass] = TOP_RIGHT;
	for (Text *letter : _compassLetters) _anchors[letter] = TOP_RIGHT;
	_anchors[_txtCompassTurn] = TOP_RIGHT;
	_anchors[_txtSprint] = TOP_RIGHT;
	_anchors[_txtEnPassant] = TOP_RIGHT;
	for (Surface *part : _panelParts) _anchors[part] = BOTTOM_LEFT;
	for (Surface *part : { _btnFly, _btnLight, _btnNight, _btnSpecial }) _anchors[part] = TOP_RIGHT;
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
	lastFlashlightOn = _flashlightOn;
	lastSprintOn = _sprintOn;
	lastEnPassantOn = _enPassantOn;
	lastPausedCheats = _pausedCheats;
	lastAkimboRealisticWasOn = _akimboRealisticWasOn;
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

	// The magnification may change with the screen size: work out how much the canvas grew,
	// move each overlay with its corner, and make the canvas and the weapon in hand again.
	const int oldWidth = (Options::baseXResolution - widthChange) / _uiScale, oldHeight = (Options::baseYResolution - heightChange) / _uiScale;
	_uiScale = uiScaleFor(Options::baseXResolution, Options::baseYResolution);
	shiftAnchored(canvasWidth() - oldWidth, canvasHeight() - oldHeight);
	makeCanvas();
	makeWeaponInHand();
	makeScopeCross();

	// A new 3D view at the new canvas size, set up like the old one and put in its place.
	FirstPersonView *newView = new FirstPersonView(_game, canvasWidth(), canvasHeight(), 0, 0);
	newView->setPalette(_state->getPalette());
	newView->setSavedBattleGame(_save);
	newView->setMap(_state->_map);
	newView->setMaxDistance(_game->getMod()->getFirstPerson().getMaxViewDistance());
	newView->setVisible(_view->getVisible());
	std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), (Surface*)_view, (Surface*)newView);
	_ownSurfaces.erase(_view);
	_ownSurfaces.insert(newView);
	_preview->shareCache(newView);
	_mapCamera->shareCache(newView);
	delete _view;
	_view = newView;
	_trackedUnit = nullptr; // re-place the camera
	makeSoldierCamView(); // (its size follows the canvas)

	// The game's warning message: its own place moved with the HUD; in full 3D it goes above
	// our info panel.
	_warningY += heightChange; // the HUD's warning place moved with the HUD
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	layoutTopBar();
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
	return std::max(1, std::min(height / 360, width / 320));
}

/**
 * Gets the width of the canvas our overlays are laid out on: the game's screen width divided
 * by the magnification (the screen's own width at scale 1).
 */
int FirstPersonMode::canvasWidth() const
{
	return Options::baseXResolution / _uiScale;
}

/**
 * Gets the height of the canvas our overlays are laid out on: the game's screen height divided
 * by the magnification.
 */
int FirstPersonMode::canvasHeight() const
{
	return Options::baseYResolution / _uiScale;
}

/**
 * Makes (again) the canvas the overlays are drawn on before being magnified onto the screen.
 * At scale 1 none is needed: everything is drawn straight onto the screen.
 */
void FirstPersonMode::makeCanvas()
{
	delete _canvas;
	_canvas = nullptr;
	if (_uiScale > 1)
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
	if (_uiScale == 1)
	{
		button->handle(action, _state);
		return;
	}

	// Where the mouse is on the canvas, turned back into window pixels as if the game's screen
	// were the canvas: the Action made from that event then reports the canvas point.
	SDL_Event event = *action->getDetails();
	const double canvasX = action->getAbsoluteXMouse() / _uiScale, canvasY = action->getAbsoluteYMouse() / _uiScale;
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
	return Options::baseYResolution - PanelH * _uiScale - 28;
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
	Timer::maxFrameSkip = Options::maxFrameSkip; // the player's own frame skipping again (see catchUp)
	_state->_map->setSoundListener(nullptr);
	_state->_map->setNightVisionHoldBlocked(false);

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
	delete _sounds;
	delete _canvas;
}

/**
 * Is the 3D view showing (in place of the isometric map)?
 */
bool FirstPersonMode::isActive() const
{
	return _view->getVisible();
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
	_wants3D = !isActive();
	if (_wants3D && _save->getSide() != FACTION_PLAYER && !Options::fpsAlienTurn3D)
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
		if (viewedUnit && !viewedUnit->isOut())
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
	for (SDLKey scrollKey : { Options::keyBattleLeft, Options::keyBattleRight, Options::keyBattleUp, Options::keyBattleDown })
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
	_state->_map->setVisible(!active);
	_state->_map->invalidate();
	_dragLook = false;
	_mouseLook = active && !_classicHud;
	_trackedUnit = nullptr; // re-place the camera on the unit

	// Keep warnings ("not enough time units"...) visible above the panel when the classic HUD is hidden.
	_state->_warning->setY(active && !_classicHud ? warningY() : _warningY);
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
 * Hides or shows the 3D HUD (the hide HUD key, P by default), for a clear view: the top bar, the
 * info panel, the compass, the lift arrows, the enemy markers and the original's enemy buttons.
 * The crosshair and the weapon in hand stay (each has its own setting), and so do the banners
 * and the game's messages. Hidden buttons can't be clicked. On the isometric map the top bar
 * shows as ever. Hidden, a message says which key brings it back.
 */
void FirstPersonMode::toggleHud()
{
	_hudHidden = !_hudHidden;
	rememberChoices();
	if (_hudHidden)
	{
		_state->warningRaw(_state->tr("STR_FPS_HUD_HIDDEN").arg(keyLabel(Options::keyFpsHideHud)));
	}
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
 * the 3D view's own light (setting "Flashlight": on, a wide beam from the shoulder in the dark;
 * dynamic, a spot on the crosshair at any time of day) and changes nothing in the game's rules;
 * the suit light (L) is the game's personal light and stays separate. Each battle starts with
 * it on.
 */
void FirstPersonMode::toggleFlashlight()
{
	// No flashlight (the setting is Off): say so, so the key doesn't seem dead.
	if (FirstPerson::flashlightMode() == 0)
	{
		_state->warning("STR_FPS_FLASHLIGHT_SETTING_OFF");
		return;
	}
	_flashlightOn = !_flashlightOn;
	rememberChoices();
	_sounds->play(FirstPersonSounds::LIGHT_SWITCH);
	// The shoulder flashlight lights only in the dark: switched by day, a line says so.
	if (FirstPerson::flashlightMode() == 1 && _save->getGlobalShade() < FlashlightDarkShade)
	{
		_state->warning("STR_FPS_FLASHLIGHT_DARK_ONLY");
	}
}

/**
 * Switches sprinting on or off (its key, Caps Lock by default). While it's on, each step in 3D
 * is the game's own run (Ctrl on the map), at a faster pace: by the default rules it costs
 * three quarters of a walk's time units and half as much energy again, and only an armor that
 * allows running runs. A soldier without the energy for a run walks the step instead (see
 * runsStep). "SPRINT" shows under the compass while it's on. Each battle starts with it off.
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
 * game's own rule, its "ignore spotted enemies"). The banner still says ENEMY SPOTTED. "EN
 * PASSANT" shows under the compass while it's on. Each battle starts with it off.
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
SDLMod FirstPersonMode::leanModifierHeld() const
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
	return (SDL_GetModState() & bits) ? (SDLMod)bits : KMOD_NONE;
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
 * Is doom mode on? It is with its own setting, and with either of its extras, which are doom
 * mode plus more: IDKFA (endless ammo) and IDDQD (invincible soldiers).
 * @return True if doom mode is on.
 */
bool FirstPersonMode::doomModeOn()
{
	return Options::fpsDoomMode || Options::fpsDoomIdkfa || Options::fpsDoomIddqd;
}

/**
 * The doom mode key (Insert): with any of the cheats on (doom mode, IDKFA, IDDQD), switches them
 * all off and remembers which were on; with none on, switches those back on (doom mode alone if
 * there are none to remember). The game's warning line says which.
 */
void FirstPersonMode::toggleCheats()
{
	if (doomModeOn())
	{
		_pausedCheats = (Options::fpsDoomMode ? 1 : 0) | (Options::fpsDoomIdkfa ? 2 : 0) | (Options::fpsDoomIddqd ? 4 : 0);
		Options::fpsDoomMode = Options::fpsDoomIdkfa = Options::fpsDoomIddqd = false;
	}
	else
	{
		const int cheats = _pausedCheats ? _pausedCheats : 1;
		Options::fpsDoomMode = (cheats & 1) != 0;
		Options::fpsDoomIdkfa = (cheats & 2) != 0;
		Options::fpsDoomIddqd = (cheats & 4) != 0;
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
 * (IDDQD, IDKFA).
 */
std::string FirstPersonMode::cheatTag() const
{
	if (!Options::fpsDoomIddqd && !Options::fpsDoomIdkfa)
	{
		return std::string(_state->tr("STR_FPS_DOOM"));
	}
	std::string tag = Options::fpsDoomIddqd ? std::string(_state->tr("STR_FPS_IDDQD")) : "";
	if (Options::fpsDoomIdkfa) tag += (tag.empty() ? "" : " ") + std::string(_state->tr("STR_FPS_IDKFA"));
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
 * Doom mode with real-time grenades (a first step towards a real-time mode): a grenade armed
 * there burns for its fuse (the setting "Doom grenade fuse" when it was armed) and then goes off
 * wherever it is: where it lies, where it lands if it's still in the air, or in the hand of
 * whoever holds it. Primed on throw (the default, see fire) it starts burning the moment it
 * leaves the hand, and one whose throw the game refused stays in the hand, armed and not
 * burning, until it does; with its pin pulled by PRIME it burns from the click, in the hand too. The explosion is
 * the game's own, as its end-of-turn fuse makes it (an ExplosionBState for a timed grenade at
 * the grenade's tile, which also removes it). One at a time, on the player's turn, when the game
 * isn't busy with a shot or another explosion. Leaving Doom mode or real-time grenades stops
 * the real-time fuses (the game's own fuse, set to 0, is still there: the grenade then goes off
 * at the end of the turn, as usual).
 * @param now The time, SDL ticks.
 */
void FirstPersonMode::updateDoomFuses(Uint32 now)
{
	if (_doomFuses.empty())
	{
		return;
	}
	if (!doomModeOn() || !Options::fpsDoomRealTimeGrenades)
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
void FirstPersonMode::think()
{
	// The frame's time: now, and the seconds since the last frame (at most a tenth of a second,
	// so a long pause doesn't make one huge step).
	const Uint32 now = SDL_GetTicks();
	lastThinkTicks = now;
	rememberChoices(); // (the fuses burning change all the time)
	const double elapsedSeconds = _lastFrameTick ? std::min<Uint32>(100, now - _lastFrameTick) / 1000.0 : 0.0;

	// Another screen was on top (popup, next turn, inventory): it had the mouse released.
	const bool resumed = _lastFrameTick && now - _lastFrameTick > 250;
	_lastFrameTick = now;

	// Explosions in 3D at the isometric map's pace (the battle's timer catching up).
	catchUp();

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

	// Doom IDDQD keeps X-COM's morale full, whatever the view and whoever's turn it is.
	applyGodMode();

	// A soldier going down or breaking down: their camera has the frame, unzoomed (the scope
	// goes).
	if (deathCam(now, elapsedSeconds) || breakdownCam(now, elapsedSeconds))
	{
		if (_zoom != 1.0)
		{
			_zoom = 1.0;
			_view->setFieldOfView(fieldOfViewNow());
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
	else if (Options::fpsAlienTurn3D && _wants3D)
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
	BattleUnit *unit = playerTurn ? selected : (Options::fpsAlienTurn3D ? (alienTurnView ? alienTurnView : _lastPlayerUnit) : nullptr);
	if (unit && unit->isOut())
	{
		unit = nullptr;
	}

	// Show the 3D view when the player wants it and there's someone to look through.
	const bool show3D = _wants3D && unit;
	if (show3D != isActive())
	{
		setActive(show3D);
	}

	// What runs in both views: the top bar, the soldier card, the map camera, the music, aiming
	// on the isometric map, akimbo's second shot in turn and flight.
	updateTopBar(selected);
	updateSoldierCard();
	updateMapCamera(); // (after the card: a squad button's soldier cam wins)
	watchMapLayers();
	updateMusic(now);
	updateMapAim();
	fireTurnShot(); // (akimbo in turn: the other gun's shot, or calling it off on the map)
	updateFlight(show3D ? unit : nullptr);

	// On the isometric map: hide the 3D view's overlays (the scope too: back in 3D the view
	// starts unzoomed), and that's all.
	if (!show3D)
	{
		_zoom = 1.0;
		updateScope();
		updateInfoPanel(nullptr);
		updateArmorButtons(nullptr);
		updateLift(nullptr, now);
		updateCompass(nullptr, false);
		updateWalkModeLines(nullptr);
		updateCrosshair(nullptr, playerTurn);
		updateUnitName(nullptr);
		updateBanner(playerTurn);
		updateWeaponInHand(nullptr);
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
	updateScope();
	_view->setFieldOfView(fieldOfViewNow());
	_view->setSmoothLighting(Options::fpsSmoothLighting);

	// What happened to the soldier and around them: a new enemy spotted, hurt, on fire, a shot
	// whizzing past; the recoil easing back, the game done aiming our last shot, and a held
	// trigger firing on.
	checkSpotted(unit, playerTurn);
	checkHurt(unit, now);
	checkBurning(unit, now);
	checkNearMiss();
	updateRecoil(unit, elapsedSeconds);
	endShotAim();
	keepFiringWhileHeld(unit, playerTurn);

	// Night vision (gear), in the isometric map's colour.
	_view->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
	// Smoke as this soldier's eyes see through it: the game's sight range, and the armor's heat
	// vision (setting "Smoke": Modern's wall, Original's stacked frames).
	_view->setSmokeSight(_save->getTileEngine()->getMaxViewDistance() * 16.0f, unit->getVisibilityThroughSmoke(), unit->getVisibilityThroughFire());
	// The flashlight (setting "Flashlight"), on and off with its own key (see toggleFlashlight):
	// on, a wide beam from the shoulder, in the dark only (the battle's shade 6 or more, of 0
	// bright .. 15 dark); dynamic, a spot on the crosshair, so it lights dark places by day too.
	const int flashlightMode = FirstPerson::flashlightMode();
	const bool dynamicFlashlight = flashlightMode == 2;
	_view->setFlashlight(flashlightMode > 0 && _flashlightOn && (dynamicFlashlight || _save->getGlobalShade() >= FlashlightDarkShade), dynamicFlashlight);

	// The camera follows the soldier (and walks them on your turn); on your turn the soldier
	// also faces where you look, and the fire mode and aim are kept up to date.
	updateCamera(unit, playerTurn, elapsedSeconds);
	if (playerTurn)
	{
		openDoorAfterTurn(unit);
		faceLookDirection(unit);
		updateFireMode(unit);
		updateAim(unit);
	}
	updateTrajectory(unit, playerTurn);

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
	updateWeaponInHand(unit);
	syncShadows();
	_view->invalidate();
}

/**
 * The soldier the camera was with is gone (killed, stunned, mind controlled...): carries on
 * with the next one, as soon as the game is free to (not busy, no popup up). Only on the
 * player's turn and while the player wants the 3D view.
 * @return The selected unit (now).
 */
BattleUnit *FirstPersonMode::selectNextSoldierIfGone(bool playerTurn)
{
	BattleUnit *selected = _save->getSelectedUnit();
	// The soldier we were with is gone (killed, stunned, mind controlled...): carry on with the next one.
	if (_wants3D && playerTurn && (!selected || selected->isOut() || selected->getFaction() != FACTION_PLAYER)
		&& !_state->_battleGame->isBusy() && _state->_popups.empty())
	{
		_state->selectNextPlayerUnit();
		selected = _save->getSelectedUnit();
	}
	return selected;
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
	const SDL_Event &event = *action->getDetails();
	const bool active = isActive();

	// Which squad button (for the soldier card) and lift arrow the free cursor is over (never consumed).
	if (event.type == SDL_MOUSEMOTION && !wantsMouseCapture())
	{
		trackHover(action);
	}

	// The mouse's thumb buttons are the game's (next and previous soldier, OXCE's option).
	if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) && (event.button.button == SDL_BUTTON_X1 || event.button.button == SDL_BUTTON_X2))
	{
		return false;
	}

	// Keys go to the key handler; anything that's neither a key nor the mouse isn't ours.
	if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
	{
		return handleKey(action, active);
	}
	if (event.type != SDL_MOUSEMOTION && event.type != SDL_MOUSEBUTTONDOWN && event.type != SDL_MOUSEBUTTONUP)
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
	_cursorX = (int)(action->getAbsoluteXMouse() / _uiScale);
	_cursorY = (int)(action->getAbsoluteYMouse() / _uiScale);
	const TextButton *hoveredButton = buttonAt(action->getAbsoluteXMouse() / _uiScale, action->getAbsoluteYMouse() / _uiScale);
	_liftArrowHovered = liftArrowAt(action->getAbsoluteXMouse() / _uiScale, action->getAbsoluteYMouse() / _uiScale);

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
	const SDLKey key = event.key.keysym.sym;
	// A key SDL can't name is never ours (an unbound action is SDLK_UNKNOWN too), nor one with
	// the game's Ctrl or Alt held (a shortcut; see gameModifierHeld).
	if (key == SDLK_UNKNOWN || gameModifierHeld())
	{
		return false;
	}
	// Went down (not up). Caps Lock and Num Lock are different: SDL 1.2 sends a lock key's
	// "down" when its lock goes on and its "up" when it goes off, one or the other for each
	// press, so for them either is a press.
	const bool lockKey = key == SDLK_CAPSLOCK || key == SDLK_NUMLOCK;
	const bool pressed = event.type == SDL_KEYDOWN || (lockKey && event.type == SDL_KEYUP);
	// (While leaning with Left Shift, the game's own hotkeys stay quiet, as the game ignores a
	// hotkey with Shift held: Q and E don't open a hand's menu in the middle of a lean.)

	// The 3D key works in both views; on the isometric map, the 3D HUD key shows or hides the
	// map camera.
	if (key == Options::keyBattleFirstPerson)
	{
		if (pressed) toggleView();
		return true;
	}
	if (!active && key == Options::keyFpsHideHud)
	{
		if (pressed) toggleMapCamera();
		return true;
	}
	if (!active)
	{
		return false;
	}

	// Our keys in 3D: the modes, fire modes and hand, doors, flares, up/down, flight, the
	// flashlight, sprinting and en passant, jump and night vision. (The flare and flashlight keys
	// with their settings off say so on the warning line.)
	if (key == Options::keyFpsMouseMode) { if (pressed) setMouseLook(!_mouseLook); return true; }
	if (key == Options::keyFpsClassicHud) { if (pressed) setClassicHud(!_classicHud); return true; }
	if (key == Options::keyFpsHideHud) { if (pressed) toggleHud(); return true; }
	if (key == Options::keyFpsDoomMode) { if (pressed) toggleCheats(); return true; }
	if (key == Options::keyFpsNextFireMode) { if (pressed) cycleFireMode(1); return true; }
	if (key == Options::keyFpsModeSnap) { if (pressed) selectFireMode(BA_SNAPSHOT); return true; }
	if (key == Options::keyFpsModeAimed) { if (pressed) selectFireMode(BA_AIMEDSHOT); return true; }
	if (key == Options::keyFpsModeAuto) { if (pressed) selectFireMode(BA_AUTOSHOT); return true; }
	if (key == Options::keyFpsSwitchHand) { if (pressed) switchHand(); return true; }
	if (key == Options::keyFpsTurnLeft) { if (pressed) { _snapTurnDegrees -= 45.0; _snapTurnFacing = true; } return true; }
	if (key == Options::keyFpsTurnRight) { if (pressed) { _snapTurnDegrees += 45.0; _snapTurnFacing = true; } return true; }
	if (key == Options::keyFpsAkimbo) { if (pressed) toggleAkimbo(); return true; }
	// The lean keys (setting "Lean") are read each frame while held (see updateLean); here they
	// are only swallowed. With leaning off they're the game's.
	if (Options::fpsLean > 0 && key != SDLK_UNKNOWN && (key == Options::keyFpsLeanHold || key == Options::keyFpsLeanLeftOwn || key == Options::keyFpsLeanRightOwn)) return true;
	if (Options::fpsZoom && key != SDLK_UNKNOWN && key == Options::keyFpsZoom) return true; // (held: see updateZoom)
	if (key == Options::keyFpsOpenDoor) { if (pressed) openDoor(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsFlare) { if (pressed) { if (Options::fpsFlares) throwFlare(); else _state->warning("STR_FPS_FLARES_OFF"); } return true; }
	if (key == Options::keyFpsUp) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_UP, action); return true; }
	if (key == Options::keyFpsDown) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_DOWN, action); return true; }
	if (key == Options::keyFpsFlight) { if (pressed) toggleFlight(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsFlashlight) { if (pressed) toggleFlashlight(); return true; }
	if (key == Options::keyFpsSprint) { if (pressed) toggleSprint(); return true; }
	if (key == Options::keyFpsEnPassant) { if (pressed) toggleEnPassant(); return true; }
	if (key == Options::keyFpsJump) { if (pressed) jump(_save->getSelectedUnit()); return true; }
	// Night vision: the gear of the soldier the view looks through (in the aliens' turn the
	// game's selected unit is the alien acting); the game's own key (Scroll Lock) checks for it
	// in 3D too.
	if (key == Options::keyFpsNightVision || key == Options::keyNightVisionToggle) { if (pressed) toggleNightVision(_trackedUnit); return true; }

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
	// sideways, relative to the view; 7 and 9 a snap turn. A press is remembered until the move
	// can start (see tileMoveVelocity), so a quick tap always counts.
	if (key == SDLK_KP8 || key == SDLK_KP2 || key == SDLK_KP4 || key == SDLK_KP6)
	{
		if (pressed && _save->getSide() == FACTION_PLAYER) // (not remembered through the aliens' turn)
		{
			_numpadMove[0] = key == SDLK_KP8 ? 1 : key == SDLK_KP2 ? -1 : 0;
			_numpadMove[1] = key == SDLK_KP6 ? 1 : key == SDLK_KP4 ? -1 : 0;
		}
		return true;
	}
	if (key == SDLK_KP7 || key == SDLK_KP9)
	{
		if (pressed)
		{
			_snapTurnDegrees += key == SDLK_KP7 ? -45.0 : 45.0;
			_snapTurnFacing = true;
		}
		return true;
	}

	// The game's "centre on enemy" keys (1 to 10) turn the view to that enemy instead (an
	// unbound key, SDLK_UNKNOWN, matches nothing).
	const SDLKey enemyKeys[] = { Options::keyBattleCenterEnemy1, Options::keyBattleCenterEnemy2, Options::keyBattleCenterEnemy3,
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
	const bool wheelTurned = event.type == SDL_MOUSEBUTTONDOWN
		&& (event.button.button == SDL_BUTTON_WHEELUP || event.button.button == SDL_BUTTON_WHEELDOWN);

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
		cycleFireMode(event.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
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
		fire(_view->getWidth() / 2, _view->getHeight() / 2);
	}
	else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
	{
		// ...and letting go releases it (in FPS-style auto, no further burst follows).
		_triggerHeld = false;
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT && !_dragLook)
	{
		// The right button forgets a target marked for confirm fire, or cancels a picked action
		// (or a walk), as on the isometric map; with nothing to cancel, it zooms while held
		// (setting "Zoom").
		if (_confirmTile.x >= 0) clearConfirmMark();
		else if (!_state->_battleGame->cancelCurrentAction()) _zoomButtonHeld = Options::fpsZoom;
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
	const bool wheelTurned = event.type == SDL_MOUSEBUTTONDOWN
		&& (event.button.button == SDL_BUTTON_WHEELUP || event.button.button == SDL_BUTTON_WHEELDOWN);

	// Moving over the map camera on the isometric map: the map's cursor stays where it was (and
	// an aimed action's target with it, which the camera looks at), not under the picture.
	if (!active && event.type == SDL_MOUSEMOTION && onMapCamera(_cursorX, _cursorY))
	{
		return true;
	}

	if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
	{
		// Where the click is, in the game's screen pixels, and which of our buttons is there.
		action->setMouseAction(event.button.x, event.button.y, 0, 0);
		const double mouseX = action->getAbsoluteXMouse(), mouseY = action->getAbsoluteYMouse();
		TextButton *buttonUnderCursor = buttonAt(mouseX / _uiScale, mouseY / _uiScale); // our buttons are on the canvas

		// Let every button see the release, so none stays drawn pressed.
		if (event.type == SDL_MOUSEBUTTONUP)
		{
			for (TextButton *button : _btnSquad) handleOnCanvas(button, action);
			for (TextButton *button : { _btnView, _btnHud, _btnInventory, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnPrev, _btnNext })
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
			browseSquad(event.button.button == SDL_BUTTON_WHEELUP ? -1 : 1, mouseX / _uiScale, mouseY / _uiScale);
			return true;
		}
		if (wheelTurned && (buttonUnderCursor == _btnPrev || buttonUnderCursor == _btnNext))
		{
			scrollSquad(event.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
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

		// On the isometric map, the map camera: a left click on it goes to the 3D view (the
		// selected soldier's), a right click opens the soldier cam in its place, or closes it. The
		// map under it gets nothing: no soldier walks off by a click on the picture.
		if (!active && onMapCamera((int)(mouseX / _uiScale), (int)(mouseY / _uiScale)))
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
		if (const int liftArrow = liftArrowAt(mouseX / _uiScale, mouseY / _uiScale)) // 1 up, -1 down
		{
			if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
			{
				moveUpDown(_save->getSelectedUnit(), liftArrow > 0 ? Pathfinding::DIR_UP : Pathfinding::DIR_DOWN, action);
			}
			return true;
		}

		// The game's own buttons, drawn on the screen itself (not on the canvas), where the map's
		// handling would move the hidden map instead: the visible enemy buttons (with or without
		// the classic HUD), and with the classic HUD, its buttons that act on the map.
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
				if (!under(_state->_btnVisibleUnit[enemyIndex])) continue;
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
		if (event.type == SDL_MOUSEBUTTONDOWN && !overClassicHud)
		{
			if (wheelTurned)
			{
				cycleFireMode(event.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
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
				fire((int)(mouseX / _uiScale), (int)(mouseY / _uiScale)); // the view is on the canvas
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
 * top bar shows over the isometric map too), and in full 3D the original's visible enemy
 * buttons. On big screens the view and the overlays are drawn on the canvas and magnified.
 * @return Always true: the screen is drawn here, so the state doesn't draw it itself.
 */
bool FirstPersonMode::blit()
{
	SDL_Surface *screen = _game->getScreen()->getSurface();
	const bool full3D = isActive() && !_classicHud;

	// The 3D view at the back, magnified on big screens: straight from the view, which is the
	// canvas's size and in the same palette (no copy onto the canvas first).
	if (_view->getVisible())
	{
		if (_canvas)
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

	// The game's own surfaces (in full 3D only its warnings and debug text).
	for (Surface *surface : _state->_surfaces)
	{
		if (!_ownSurfaces.count(surface) && (!full3D || surface == _state->_warning || surface == _state->_txtDebug))
		{
			surface->blit(screen);
		}
	}

	// Our overlays: straight onto the screen, or onto the cleared canvas and magnified (its
	// empty pixels let what's under them show).
	if (!_canvas)
	{
		for (Surface *surface : _state->_surfaces)
		{
			if (surface != _view && _ownSurfaces.count(surface) && !(hudHidden() && _topBarSurfaces.count(surface))) surface->blit(screen);
		}
	}
	else
	{
		_canvas->clear();
		for (Surface *surface : _state->_surfaces)
		{
			if (surface != _view && _ownSurfaces.count(surface) && !(hudHidden() && _topBarSurfaces.count(surface))) surface->blit(_canvas->getSurface());
		}
		magnify(screen, _canvas->getSurface(), true);
	}

	// The original's visible enemy buttons (bottom right; clicking one turns the view to it),
	// with their numbers, when the classic HUD that normally has them is hidden (and the 3D HUD
	// isn't).
	if (full3D && !hudHidden())
	{
		for (int enemyIndex = 0; enemyIndex < BattlescapeState::VISIBLE_MAX; ++enemyIndex)
		{
			if (_state->_btnVisibleUnit[enemyIndex]->getVisible())
			{
				_state->_btnVisibleUnit[enemyIndex]->blit(screen);
				_state->_numVisibleUnit[enemyIndex]->blit(screen);
			}
		}
	}
	return true;
}

/**
 * Copies a canvas-sized surface (the canvas with the overlays, or the 3D view itself) onto the
 * screen _uiScale times bigger, each of its pixels becoming a square of _uiScale x _uiScale
 * screen pixels (all are 8-bit surfaces in the state's palette: the colours are copied as they are).
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
