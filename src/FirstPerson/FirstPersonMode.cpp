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
#include <cmath>
#include <string>
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Interface/Bar.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

namespace
{

// The game remakes the battlescape screen (and with it this mode) when options change
// mid-battle; what the player had chosen (3D or not, the classic HUD) carries over to the new
// screen of the same battle. Loading a game from inside a battle remakes the screen too, with a
// new battle object: that carries over as well, when the old screen ran only a moment ago.

/// The battle the choices below were made in.
const SavedBattleGame *lastBattle = nullptr;
/// Whether the player had switched to the 3D view (FirstPersonMode::_wants3D).
bool lastWants3D = false;
/// Whether the classic HUD was shown over the 3D view (FirstPersonMode::_classicHud).
bool lastClassicHud = false;
/// When a first-person mode last ran its per-frame update (SDL ticks, ms; 0: never).
Uint32 lastThinkTicks = 0;

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
	_game(game), _state(state), _save(state->_save), _wants3D(Options::fpsStartInFirstPerson), _warningY(state->_warning->getY())
{
	// The overlays are laid out on a canvas; on big screens it's drawn magnified (see blit).
	// Whether each step is allowed is not known yet (see stepAllowed).
	_uiScale = uiScaleFor(Options::baseXResolution, Options::baseYResolution);
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);

	// Our surfaces, in drawing order: each one goes on top of the ones made before it.
	createView();
	createCrosshairAndBanner();
	createLiftArrows();
	createTopBar();
	createInfoPanel();
	createSoldierCard();

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

	// Which corner each surface keeps to on a resize, then the top bar's buttons laid out.
	setResizeAnchors();

	layoutTopBar();

	// A screen remade for the same battle, or right after another one (a game loaded from
	// inside a battle, within 3 seconds), takes over the player's choices.
	if (lastBattle == _save || (lastThinkTicks && SDL_GetTicks() - lastThinkTicks < 3000))
	{
		_wants3D = lastWants3D;
		_classicHud = lastClassicHud;
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

	// All hidden until there's something to show.
	for (Text *text : { _txtMode, _txtShot, _txtBanner })
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
 * Makes the info panel, bottom left: name, DOOM (the debug mode's tag), weapon, status, the
 * four stats (time units, energy, health, morale) as numbers and bars coloured like the
 * original HUD, and the armor's name. It has no background, so the world shows through (the
 * texts have dark shadows). Filled in by updateInfoPanel.
 */
void FirstPersonMode::createInfoPanel()
{
	const int height = canvasHeight(); // our surfaces are laid out on the canvas (see makeCanvas)

	// The panel's top left corner; everything in it is placed from there (canvas pixels).
	const int panelX = 1, panelY = height - PanelH - 1;

	// First line: the name, and DOOM on the right. Second line: the weapon, and the status on
	// the right.
	_txtName = addText(PanelW - 66, 9, panelX + 3, panelY + 2, "textName", "battlescape");
	_txtDoom = addText(60, 9, panelX + PanelW - 63, panelY + 2, "doom", "firstPerson");
	_txtWeapon = addText(PanelW - 46, 9, panelX + 3, panelY + 11, "weapon", "firstPerson");
	_txtStatus = addText(40, 9, panelX + PanelW - 43, panelY + 11, "weapon", "firstPerson");

	// The four stats in two columns, each a number with its bar beside it, in the original
	// HUD's colours.
	_numTimeUnits = addNumber(panelX + 3, panelY + 21, "numTUs");
	_barTimeUnits = new Bar(52, 3, panelX + 19, panelY + 22);
	_numEnergy = addNumber(panelX + 76, panelY + 21, "numEnergy");
	_barEnergy = new Bar(52, 3, panelX + 92, panelY + 22);
	_numHealth = addNumber(panelX + 3, panelY + 27, "numHealth");
	_barHealth = new Bar(52, 3, panelX + 19, panelY + 28);
	_numMorale = addNumber(panelX + 76, panelY + 27, "numMorale");
	_barMorale = new Bar(52, 3, panelX + 92, panelY + 28);
	_state->add(_barTimeUnits, "barTUs", "battlescape");
	_state->add(_barEnergy, "barEnergy", "battlescape");
	_state->add(_barHealth, "barHealth", "battlescape");
	_state->add(_barMorale, "barMorale", "battlescape");

	// Last line: the armor.
	_txtArmor = addText(PanelW - 6, 9, panelX + 3, panelY + 35, "weapon", "firstPerson");

	// The panel's parts, all ours and all hidden until the 3D view shows.
	_panelParts = { _txtName, _txtDoom, _txtWeapon, _txtStatus, _numTimeUnits, _barTimeUnits, _numEnergy, _barEnergy,
		_numHealth, _barHealth, _numMorale, _barMorale, _txtArmor };
	for (auto *part : _panelParts)
	{
		_ownSurfaces.insert(part);
		part->setVisible(false);
	}

	// Right-aligned texts, the fixed DOOM tag, and bars at one pixel per point.
	_txtDoom->setAlign(ALIGN_RIGHT);
	_txtStatus->setAlign(ALIGN_RIGHT);
	_txtDoom->setText(_state->tr("STR_FPS_DOOM"));
	for (auto *bar : { _barTimeUnits, _barEnergy, _barHealth, _barMorale })
	{
		bar->setScale(1.0);
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
	_preview->setSavedBattleGame(_save);
	_preview->setMap(_state->_map);
	_preview->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	_preview->shareCache(_view);
	_preview->setParticles(false);
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

	// END and the suit's buttons keep to the top right; the info panel to the bottom left.
	_anchors[_btnEndTurn] = TOP_RIGHT;
	for (Surface *part : _panelParts) _anchors[part] = BOTTOM_LEFT;
	for (Surface *part : { _btnFly, _btnLight, _btnNight, _btnSpecial }) _anchors[part] = TOP_RIGHT;
}


/**
 * Notes what the player has chosen (3D or not, the classic HUD), for a remade screen of the
 * same battle. Called the moment a choice is made: the old screen is only deleted after the new
 * one is made, so waiting for its destructor would be too late.
 */
void FirstPersonMode::rememberChoices() const
{
	lastBattle = _save;
	lastWants3D = _wants3D;
	lastClassicHud = _classicHud;
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
	delete _view;
	_view = newView;
	_trackedUnit = nullptr; // re-place the camera

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
	_state->_map->setSoundListener(nullptr);
	_state->_map->setNightVisionHoldBlocked(false);

	// A quickload deletes the battle before this screen goes; only a battle that lives on (the
	// screen remade after an options change) gets its pathfinding back to the rules (no vault
	// let through).
	SavedGame *savedGame = _game->getSavedGame();
	if (savedGame && savedGame->getSavedBattle() == _save)
	{
		_save->getPathfinding()->setVault(nullptr, {});
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
 * Switches between the isometric map and the 3D view (the 3D key or button). The switch
 * itself happens in think(), called right away, which also checks there is a soldier to look
 * through.
 */
void FirstPersonMode::toggleView()
{
	_wants3D = !_wants3D;
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
	// own (the setting "Flight cost per level"), no vault in progress.
	if (!active)
	{
		_save->getPathfinding()->setFlightCost(nullptr, -1); // by the rules on the isometric map
		endVault();
	}

	// In 3D the night vision hold key (Space) jumps; night vision there is gear with its own key.
	_state->_map->setNightVisionHoldBlocked(active && Options::keyFpsJump == Options::keyNightVisionHold);

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
 * Switches between mouse look (the mouse turns the view, captured) and a free cursor (for
 * clicking our buttons and aiming at a point). Ends any right-button drag look.
 * @param on True for mouse look.
 */
void FirstPersonMode::setMouseLook(bool on)
{
	_mouseLook = on;
	_dragLook = false;
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
 * Doom mode (debug): the selected soldier never runs out of time units, energy or ammo. Called
 * every frame: tops up the time units and energy, and refills every clip loaded in the weapons
 * in both hands.
 */
void FirstPersonMode::applyDoomMode(BattleUnit *unit)
{
	unit->resetTimeUnitsAndEnergy();
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
	const double elapsedSeconds = _lastFrameTick ? std::min<Uint32>(100, now - _lastFrameTick) / 1000.0 : 0.0;

	// Another screen was on top (popup, next turn, inventory): it had the mouse released.
	const bool resumed = _lastFrameTick && now - _lastFrameTick > 250;
	_lastFrameTick = now;

	// When the mouse gets captured or released (or we come back from another screen), the
	// cursor jumps; ignore mouse movement for a moment so the jump doesn't turn the view.
	if (wantsMouseCapture() != _mouseWasCaptured || resumed)
	{
		_mouseWasCaptured = wantsMouseCapture();
		_ignoreMotionUntil = now + 150;
	}

	// A soldier going down or breaking down: their camera has the frame.
	if (deathCam(now, elapsedSeconds) || breakdownCam(now, elapsedSeconds))
	{
		return;
	}

	// Whose eyes to look through: the selected soldier on your turn, your last soldier on the
	// aliens' turn (if that's switched on), nobody if they're out.
	const bool playerTurn = _save->getSide() == FACTION_PLAYER;
	BattleUnit *selected = selectNextSoldierIfGone(playerTurn);
	updateLastPlayerUnit(playerTurn, selected);
	BattleUnit *unit = playerTurn ? selected : (Options::fpsAlienTurn3D ? _lastPlayerUnit : nullptr);
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

	// What runs in both views: the top bar, the soldier card, the music, aiming on the
	// isometric map and flight.
	updateTopBar(selected);
	updateSoldierCard();
	updateMusic(now);
	updateMapAim();
	updateFlight(show3D ? unit : nullptr);

	// On the isometric map: hide the 3D view's overlays, and that's all.
	if (!show3D)
	{
		updateInfoPanel(nullptr);
		updateArmorButtons(nullptr);
		updateLift(nullptr, now);
		updateCrosshair(nullptr, playerTurn);
		updateBanner(playerTurn);
		updateWeaponInHand(nullptr);
		syncShadows();
		return;
	}

	// Doom mode (debug) tops the soldier up, on your turn.
	if (playerTurn && Options::fpsDoomMode && unit->getFaction() == FACTION_PLAYER)
	{
		applyDoomMode(unit);
	}

	// Turning with the keys, and the view's settings (they can change in the options).
	turnWithKeys(elapsedSeconds);
	_view->setFieldOfView(Options::fpsFieldOfView);
	_view->setSmoothLighting(Options::fpsSmoothLighting);

	// What happened to the soldier and around them: a new enemy spotted, hurt, on fire, a shot
	// whizzing past; the recoil easing back, and a held trigger firing on.
	checkSpotted(unit, playerTurn);
	checkHurt(unit, now);
	checkBurning(unit, now);
	checkNearMiss();
	updateRecoil(unit, elapsedSeconds);
	keepFiringWhileHeld(unit, playerTurn);

	// Night vision (gear), in the isometric map's colour. In the dark (the battle's shade 6 or
	// more, of 0 bright .. 15 dark), the soldier's personal light becomes a shoulder flashlight
	// (setting "Flashlight").
	_view->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
	_view->setFlashlight(Options::fpsFlashlight && _save->getTogglePersonalLight() && _save->getGlobalShade() >= 6);

	// The camera follows the soldier (and walks them on your turn); on your turn the soldier
	// also faces where you look, and the fire mode and aim are kept up to date.
	updateCamera(unit, playerTurn, elapsedSeconds);
	if (playerTurn)
	{
		faceLookDirection(unit);
		updateFireMode(unit);
		updateAim(unit);
	}

	// Everything drawn over the view, then the view itself is redrawn.
	updateEnemyMarkers();
	updateInfoPanel(unit);
	updateArmorButtons(unit);
	updateLift(unit, now);
	updateCrosshair(unit, playerTurn);
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
	// Where the mouse is, on the canvas our buttons and arrows live on.
	const SDL_Event &event = *action->getDetails();
	action->setMouseAction(event.motion.x, event.motion.y, 0, 0);
	const TextButton *hoveredButton = buttonAt(action->getAbsoluteXMouse() / _uiScale, action->getAbsoluteYMouse() / _uiScale);
	_liftArrowHovered = liftArrowAt(action->getAbsoluteXMouse() / _uiScale, action->getAbsoluteYMouse() / _uiScale);

	// Which squad button that is, if any (-1: none).
	_hoveredSquadButton = -1;
	for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
	{
		if (hoveredButton == _btnSquad[buttonIndex]) _hoveredSquadButton = (int)buttonIndex;
	}
}

/**
 * A key went down or up: the view toggle anywhere; in 3D our own keys. Each of our keys acts
 * when it goes down, and both its down and up are ours. The movement keys are polled each
 * frame, and swallowed here so they don't fire battlescape hotkeys. With Ctrl or Alt held the
 * key belongs to the game.
 * @param active Whether the 3D view is showing.
 * @return True if the key was ours.
 */
bool FirstPersonMode::handleKey(Action *action, bool active)
{
	const SDL_Event &event = *action->getDetails();
	if (_game->isCtrlPressed() || _game->isAltPressed())
	{
		return false;
	}
	const SDLKey key = event.key.keysym.sym;
	const bool pressed = event.type == SDL_KEYDOWN; // went down (not up)

	// The 3D key works in both views.
	if (key == Options::keyBattleFirstPerson)
	{
		if (pressed) toggleView();
		return true;
	}
	if (!active)
	{
		return false;
	}

	// Our keys in 3D: the modes, fire modes and hand, doors, flares, up/down, flight, jump and
	// night vision. (The flare key is only ours while flares are switched on.)
	if (key == Options::keyFpsMouseMode) { if (pressed) setMouseLook(!_mouseLook); return true; }
	if (key == Options::keyFpsClassicHud) { if (pressed) setClassicHud(!_classicHud); return true; }
	if (key == Options::keyFpsDoomMode) { if (pressed) Options::fpsDoomMode = !Options::fpsDoomMode; return true; }
	if (key == Options::keyFpsNextMode) { if (pressed) cycleFireMode(1); return true; }
	if (key == Options::keyFpsModeSnap) { if (pressed) selectFireMode(BA_SNAPSHOT); return true; }
	if (key == Options::keyFpsModeAimed) { if (pressed) selectFireMode(BA_AIMEDSHOT); return true; }
	if (key == Options::keyFpsModeAuto) { if (pressed) selectFireMode(BA_AUTOSHOT); return true; }
	if (key == Options::keyFpsSwitchHand) { if (pressed) switchHand(); return true; }
	if (key == Options::keyFpsOpenDoor) { if (pressed) openDoor(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsFlare && Options::fpsFlares) { if (pressed) throwFlare(); return true; }
	if (key == Options::keyFpsUp) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_UP, action); return true; }
	if (key == Options::keyFpsDown) { if (pressed) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_DOWN, action); return true; }
	if (key == Options::keyFpsFlight) { if (pressed) toggleFlight(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsJump) { if (pressed) jump(_save->getSelectedUnit()); return true; }
	if (key == Options::keyFpsNightVision) { if (pressed) toggleNightVision(_save->getSelectedUnit(), action); return true; }

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
		// window centre isn't a look).
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
		// The middle button frees the cursor.
		setMouseLook(false);
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT && !_dragLook)
	{
		// The left button pulls the trigger: a shot at the middle of the view (the crosshair).
		_triggerHeld = true;
		fire(_view->getWidth() / 2, _view->getHeight() / 2);
	}
	else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
	{
		// ...and letting go releases it (in FPS-style auto, no further burst follows).
		_triggerHeld = false;
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT && !_dragLook)
	{
		// The right button cancels a picked action, as on the isometric map.
		_state->_battleGame->cancelCurrentAction();
	}
	else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_RIGHT && _dragLook)
	{
		// Letting go of the right button ends a drag look (back to the free cursor).
		_dragLook = false;
	}
	return true;
}

/**
 * The mouse with a free cursor: our buttons get it first, then the lift arrows and the visible
 * enemy buttons; over the view a left click fires at the cursor, the right button drags the
 * view round and the middle button goes back to mouse look. The wheel scrolls the squad
 * buttons when over them, and changes the fire mode over the view.
 * @param active Whether the 3D view is showing (on the isometric map only our buttons count).
 * @return True if the event was consumed.
 */
bool FirstPersonMode::handleFreeCursor(Action *action, bool active)
{
	const SDL_Event &event = *action->getDetails();
	const bool wheelTurned = event.type == SDL_MOUSEBUTTONDOWN
		&& (event.button.button == SDL_BUTTON_WHEELUP || event.button.button == SDL_BUTTON_WHEELDOWN);

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

		// The wheel over the squad buttons (or previous / next) scrolls them.
		const bool overSquad = buttonUnderCursor && (buttonUnderCursor == _btnPrev || buttonUnderCursor == _btnNext
			|| std::find(_btnSquad.begin(), _btnSquad.end(), buttonUnderCursor) != _btnSquad.end());
		if (wheelTurned && overSquad)
		{
			scrollSquad(event.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
			return true;
		}

		// Any click on one of our buttons is ours; a left press clicks it.
		if (buttonUnderCursor)
		{
			if (event.type == SDL_MOUSEBUTTONDOWN)
			{
				handleOnCanvas(buttonUnderCursor, action); // pressed look and click sound
				if (event.button.button == SDL_BUTTON_LEFT)
				{
					buttonClicked(buttonUnderCursor, action);
				}
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

		// The visible enemy buttons turn the view to that enemy (the classic HUD's own handling
		// centres the map). They are the game's, drawn on the screen itself, not on the canvas.
		if (!_classicHud && event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
		{
			for (int enemyIndex = 0; enemyIndex < BattlescapeState::VISIBLE_MAX; ++enemyIndex)
			{
				const InteractiveSurface *enemyButton = _state->_btnVisibleUnit[enemyIndex];
				if (enemyButton->getVisible()
					&& mouseX >= enemyButton->getX() && mouseX < enemyButton->getX() + enemyButton->getWidth()
					&& mouseY >= enemyButton->getY() && mouseY < enemyButton->getY() + enemyButton->getHeight())
				{
					lookAtEnemy(enemyIndex);
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
				setMouseLook(true);
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

	// The 3D view at the back, magnified on big screens.
	if (_view->getVisible())
	{
		if (_canvas)
		{
			_view->blit(_canvas->getSurface());
			magnify(screen, false);
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
			if (surface != _view && _ownSurfaces.count(surface)) surface->blit(screen);
		}
	}
	else
	{
		_canvas->clear();
		for (Surface *surface : _state->_surfaces)
		{
			if (surface != _view && _ownSurfaces.count(surface)) surface->blit(_canvas->getSurface());
		}
		magnify(screen, true);
	}

	// The original's visible enemy buttons (bottom right; clicking one turns the view to it),
	// with their numbers, when the classic HUD that normally has them is hidden.
	if (full3D)
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
 * Copies the canvas onto the screen _uiScale times bigger, each canvas pixel becoming a square
 * of _uiScale x _uiScale screen pixels (both are 8-bit palette surfaces).
 * @param screen The screen surface to draw on.
 * @param transparent True to skip the canvas's empty pixels (colour 0), so what's on the screen
 * shows through them; false to copy every pixel.
 */
void FirstPersonMode::magnify(SDL_Surface *screen, bool transparent)
{
	// Keep the canvas's palette the same as the state's (it changes, e.g. with the time of day).
	const SDL_Color *palette = _state->getPalette();
	if (!std::equal(palette, palette + 256, _canvasPalette, [](const SDL_Color &stateColor, const SDL_Color &canvasColor)
		{ return stateColor.r == canvasColor.r && stateColor.g == canvasColor.g && stateColor.b == canvasColor.b; }))
	{
		std::copy(palette, palette + 256, _canvasPalette);
		_canvas->setPalette(_canvasPalette);
	}

	// The part of the screen the magnified canvas covers (it can be a few pixels short of the
	// screen, when the screen isn't a multiple of the scale).
	SDL_Surface *canvasSurface = _canvas->getSurface();
	const int drawWidth = std::min(screen->w, canvasSurface->w * _uiScale), drawHeight = std::min(screen->h, canvasSurface->h * _uiScale);
	SDL_LockSurface(screen);
	SDL_LockSurface(canvasSurface);

	// Each canvas row is widened once, then laid on its _uiScale screen rows (copied whole
	// where opaque).
	_magnifyRow.resize(drawWidth);
	for (int canvasY = 0; canvasY * _uiScale < drawHeight; ++canvasY)
	{
		// Widen the row: every canvas pixel repeated _uiScale times.
		const Uint8 *canvasRow = (const Uint8 *)canvasSurface->pixels + canvasY * canvasSurface->pitch;
		for (int screenX = 0, canvasX = 0; screenX < drawWidth; ++canvasX)
		{
			for (int repeat = 0; repeat < _uiScale && screenX < drawWidth; ++repeat) _magnifyRow[screenX++] = canvasRow[canvasX];
		}

		// Lay it on each of its screen rows: all of it, or only its coloured pixels.
		for (int screenY = canvasY * _uiScale; screenY < std::min(drawHeight, (canvasY + 1) * _uiScale); ++screenY)
		{
			Uint8 *screenRow = (Uint8 *)screen->pixels + screenY * screen->pitch;
			if (!transparent)
			{
				std::copy(_magnifyRow.begin(), _magnifyRow.end(), screenRow);
				continue;
			}
			for (int screenX = 0; screenX < drawWidth; ++screenX)
			{
				if (_magnifyRow[screenX]) screenRow[screenX] = _magnifyRow[screenX];
			}
		}
	}
	SDL_UnlockSurface(canvasSurface);
	SDL_UnlockSurface(screen);
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
