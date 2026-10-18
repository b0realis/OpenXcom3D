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
#include "FirstPersonMode.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <sstream>
#include <string>
#include "FirstPersonSounds.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitFallBState.h"
#include "../Battlescape/UnitTurnBState.h"
#include "../Battlescape/UnitWalkBState.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Engine/Screen.h"
#include "../Engine/Sound.h"
#include "../Engine/SurfaceSet.h"
#include "../Interface/Bar.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{

namespace
{

// layout of the top bar, the info panel and the crosshair, in screen pixels at the battlescape's base resolution
const int BarY = 1, BarH = 12;
const int ViewButtonX = 1, HudButtonX = 24, InventoryButtonX = 47, ToolButtonW = 22;
const int SquadX = 72;
const int EndTurnW = 30;
const int PrevNextW = 10;
const int SquadWindow = 10, SquadMinW = 13; // squad buttons shown at once, and their least width
/// The weapon in hand is seen through the default 75 degree lens whatever the view's field of
/// view (1 / tan 37.5 = 1.303 times the 90 degree one).
const double WeaponLens = 1.303;
// where the weapon's muzzle (its bore) sits, for the right hand (the left mirrors it): right of
// and below the crosshair, as a direction (x / depth, y / depth); through the lens that's 0.42
// half-widths of the view across and 0.45 down
const double MuzzleScreen[2] = { 0.42 / WeaponLens, 0.45 / WeaponLens };
/// How much of the line from the stock point to the muzzle the weapon in hand fills.
const double WeaponScale = 0.9;
// a weapon shorter than the rifle (its picture is about this long) comes out smaller: by its
// length to this power (a pistol, half a rifle's picture, at 70%), shrunk about a point this far
// from the muzzle towards the stock end (it still points at the crosshair)
const double RiflePixels = 41.0;
const double SizeCurve = 0.6;
const double GripAt = 0.3;
const int PanelW = 150, PanelH = 45;
const int CrosshairSize = 41;
const int CardW = 124, CardY = 15, PreviewW = 116, PreviewH = 72;

/// How close (in voxels) the camera may get to a tile edge it can't cross.
const double EdgeMargin = 5.0;
/// How long the camera waits in front of a door it opened (the 3D view swings it open meanwhile).
const Uint32 DoorWait = 150;
/// How far from a closed door the camera stops (from the tile edge, in voxels).
const double DoorMargin = 11.0;
/// How long the view stays with a soldier who went down (killed, unconscious) or was taken over,
/// and how long the next soldier's view takes to come in out of it.
const Uint32 DeathCamTime = 2600, FaintTime = 2400, ControlledTime = 2400, AfterFallTime = 600;
/// The lift arrows (canvas pixels) and how long a ride up or down one level takes.
const int LiftArrowW = 48, LiftArrowH = 14;
const Uint32 RideTime = 1400, DropTime = 600; // a lift ride (or flying up/down); a fall or landing
/// A melee swing: wind up, strike, back (ms).
const Uint32 SwingTime = 420;
/// A jump: a free hop this high (voxels) and long; a vault over something low arcs higher.
const double JumpHeight = 6.0, VaultHeight = 9.0;
const Uint32 JumpTime = 520;
/// The highest obstacle a soldier can jump over, in voxels: waist high (a soldier is about 22).
const int VaultMaxHeight = 14;

const double Pi = 3.14159265358979323846;

/// The game remakes the battlescape screen when options change mid-battle; what the player
/// had (3D or not, classic HUD) carries over to the new screen of the same battle.
/// Loading a game from inside a battle remakes the screen too: that carries over as well.
const SavedBattleGame *lastBattle = nullptr;
bool lastWanted = false, lastClassicHud = false;
Uint32 lastAlive = 0;

/// Nearest colour of a palette (index 0 is transparent and never picked).
Uint8 nearestColor(const SDL_Color *pal, int r, int g, int b)
{
	int best = 1, bestDist = INT_MAX;
	for (int i = 1; i < 256; ++i)
	{
		const int dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
		const int dist = dr * dr * 3 + dg * dg * 6 + db * db;
		if (dist < bestDist)
		{
			bestDist = dist;
			best = i;
		}
	}
	return (Uint8)best;
}

/// Base of the colour group whose text shades look most like this colour: Text draws
/// glyphs as base + 3, 6, 9 (high contrast), which must stay inside one 16-colour group.
Uint8 textColor(const SDL_Color *pal, int r, int g, int b)
{
	int best = 16, bestDist = INT_MAX;
	for (int group = 0; group < 16; ++group)
	{
		const SDL_Color &c = pal[group * 16 + 3];
		const int dr = c.r - r, dg = c.g - g, db = c.b - b;
		const int dist = dr * dr * 3 + dg * dg * 6 + db * db;
		if (dist < bestDist)
		{
			bestDist = dist;
			best = group * 16;
		}
	}
	return (Uint8)best;
}

bool isShot(BattleActionType mode)
{
	return mode == BA_SNAPSHOT || mode == BA_AIMEDSHOT || mode == BA_AUTOSHOT;
}

}

/**
 * Creates the first-person view and the new GUI, and adds them to the battlescape state.
 * The view goes right above the isometric map, so the classic HUD still draws over it;
 * the crosshair, top bar and panel go on top of everything.
 * @param game Pointer to the core game.
 * @param state The battlescape screen.
 */
FirstPersonMode::FirstPersonMode(Game *game, BattlescapeState *state) :
	_game(game), _state(state), _save(state->_save), _hudGroup(nullptr), _squadGroup(nullptr),
	_wanted(Options::fpsStartInFirstPerson), _classicHud(false), _mouseLook(true), _dragLook(false),
	_lastTick(0), _warningY(state->_warning->getY()), _captured(false), _ignoreMotionUntil(0),
	_trackedUnit(nullptr), _lastPlayerUnit(nullptr), _posX(0.0), _posY(0.0), _eyeZ(0.0), _stepPending(false),
	_throughDoor(false), _doorStep{ 0, 0 }, _doorStepStart(0),
	_spotter(nullptr), _spottedCount(0), _movementHeld(false), _spottedUntil(0),
	_healthUnit(nullptr), _lastHealth(0), _hurtStart(0), _hurtAmount(0.0f), _deathUnit(nullptr), _deathStart(0),
	_footstepDistance(0.0), _footstepPhase(0),
	_mode(BA_NONE), _modeWeapon(nullptr), _aimValid(false), _crosshairKey(0xFFFFFFFF)
{
	// the overlays are laid out on a canvas; on big screens it's drawn magnified (see blit)
	_ui = uiScaleFor(Options::baseXResolution, Options::baseYResolution);
	const int screenWidth = canvasWidth();
	const int screenHeight = canvasHeight();
	const Mod *mod = _game->getMod();
	std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);

	// 3D view: full screen, inserted right after the isometric map it replaces; it renders at the
	// canvas size too and is magnified with the overlays (big screens cost what 640x400 does)
	_view = new FirstPersonView(_game, screenWidth, screenHeight, 0, 0);
	_state->add(_view);
	auto &surfaces = _state->_surfaces;
	surfaces.pop_back();
	surfaces.insert(std::find(surfaces.begin(), surfaces.end(), _state->_map) + 1, _view);
	_view->setSavedBattleGame(_save);
	_view->setMap(_state->_map);
	_view->setMaxDistance(mod->getFirstPerson().getMaxViewDistance());
	_view->setVisible(false);
	_own.insert(_view);

	// crosshair and its labels, in the middle of the screen
	_crosshair = new Surface(CrosshairSize, CrosshairSize, screenWidth / 2 - CrosshairSize / 2, screenHeight / 2 - CrosshairSize / 2);
	_state->add(_crosshair);
	const int glow[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
	for (const auto &offset : glow)
	{
		Text *text = new Text(240, 17, screenWidth / 2 - 120 + offset[0], 24 + offset[1]);
		_state->add(text);
		text->setBig();
		text->setAlign(ALIGN_CENTER);
		text->setVisible(false);
		_own.insert(text);
		_bannerGlow.push_back(text);
	}
	_txtBanner = addText(240, 17, screenWidth / 2 - 120, 24, "banner", "firstPerson");
	_txtBanner->setBig();
	_shadows.back()->setBig();
	_txtFallName = addText(240, 9, screenWidth / 2 - 120, 43, "banner", "firstPerson");
	_txtFallName->setAlign(ALIGN_CENTER);
	_txtFallName->setVisible(false);
	// mode and hit chance to the right of the crosshair, so they don't cover what you aim at
	_txtMode = addText(120, 9, screenWidth / 2 + 14, screenHeight / 2 - 9, "crosshairText", "firstPerson");
	_txtShot = addText(120, 9, screenWidth / 2 + 14, screenHeight / 2 + 1, "crosshairText", "firstPerson");
	_txtBanner->setAlign(ALIGN_CENTER);
	for (Text *text : { _txtMode, _txtShot, _txtBanner })
	{
		text->setVisible(false);
	}
	_crosshair->setVisible(false);
	_own.insert(_crosshair);

	// lift arrows with LIFT PLATFORM between them, right of the middle of the view
	const int liftX = screenWidth * 82 / 100;
	_liftUp = new Surface(LiftArrowW, LiftArrowH, liftX - LiftArrowW / 2, screenHeight / 2 - 38);
	_liftDown = new Surface(LiftArrowW, LiftArrowH, liftX - LiftArrowW / 2, screenHeight / 2 + 24);
	for (Surface *arrow : { _liftUp, _liftDown })
	{
		_state->add(arrow);
		arrow->setVisible(false);
		_own.insert(arrow);
	}
	_txtLift = addText(100, 9, liftX - 50, screenHeight / 2 - 18, "weapon", "firstPerson");
	_txtLiftKeys = addText(100, 9, liftX - 50, screenHeight / 2 - 8, "weapon", "firstPerson");
	_txtLift->setText(_state->tr("STR_FPS_LIFT"));
	const Uint8 lilac = textColor(_state->getPalette(), 200, 120, 255); // a group's start: the glyph shades run down it
	for (Text *text : { _txtLift, _txtLiftKeys })
	{
		text->setAlign(ALIGN_CENTER);
		text->setColor(lilac);
		text->setVisible(false);
	}

	// top bar: small buttons in the style of the original UI; colours come from the mod's "firstPerson" interface
	auto addButton = [&](int x, int w, const std::string &label) { return makeButton(x, w, label); };
	_btnView = addButton(ViewButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_3D"));
	_btnHud = addButton(HudButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_HUD"));
	_btnInventory = addButton(InventoryButtonX, ToolButtonW, _state->tr("STR_FPS_BUTTON_INVENTORY"));
	_btnEndTurn = addButton(screenWidth - 1 - EndTurnW, EndTurnW, _state->tr("STR_FPS_BUTTON_END_TURN"));
	_btnHud->setGroup(&_hudGroup);
	_btnPrev = addButton(SquadX, PrevNextW, "<");
	_btnNext = addButton(SquadX, PrevNextW, ">");
	// the suit's buttons, shown for what it can do (laid out right to left from END)
	const bool underwater = _save->getDepth() > 0;
	const char *suitLabels[4][2] = { { underwater ? "STR_FPS_BUTTON_SWIM" : "STR_FPS_BUTTON_FLY", underwater ? "STR_FPS_BUTTON_SWIM_SHORT" : "STR_FPS_BUTTON_FLY_SHORT" },
		{ "STR_FPS_BUTTON_LIGHT", "STR_FPS_BUTTON_LIGHT_SHORT" }, { "STR_FPS_BUTTON_NIGHT", "STR_FPS_BUTTON_NIGHT_SHORT" },
		{ "STR_FPS_BUTTON_SPECIAL", "STR_FPS_BUTTON_SPECIAL_SHORT" } };
	TextButton **suit[4] = { &_btnFly, &_btnLight, &_btnNight, &_btnSpecial };
	for (int k = 0; k < 4; ++k)
	{
		_suitLong[k] = _state->tr(suitLabels[k][0]);
		_suitShort[k] = _state->tr(suitLabels[k][1]);
		*suit[k] = addButton(0, labelWidth(_suitLong[k]), _suitLong[k]);
		(*suit[k])->setVisible(false);
	}
	_btnFly->setGroup(&_flyGroup);
	_btnLight->setGroup(&_lightGroup);
	_btnNight->setGroup(&_nightGroup);
	// one squad button per soldier, made as they're needed (see refreshSquad)

	// info panel, bottom left: name, weapon, the four stats, coloured like the original HUD;
	// no background, so the world shows through (the texts have dark shadows)
	const int px = 1, py = screenHeight - PanelH - 1;
	_txtName = addText(PanelW - 66, 9, px + 3, py + 2, "textName", "battlescape");
	_txtDoom = addText(60, 9, px + PanelW - 63, py + 2, "doom", "firstPerson");
	_txtWeapon = addText(PanelW - 46, 9, px + 3, py + 11, "weapon", "firstPerson");
	_txtStatus = addText(40, 9, px + PanelW - 43, py + 11, "weapon", "firstPerson");
	_numTimeUnits = addNumber(px + 3, py + 21, "numTUs");
	_barTimeUnits = new Bar(52, 3, px + 19, py + 22);
	_numEnergy = addNumber(px + 76, py + 21, "numEnergy");
	_barEnergy = new Bar(52, 3, px + 92, py + 22);
	_numHealth = addNumber(px + 3, py + 27, "numHealth");
	_barHealth = new Bar(52, 3, px + 19, py + 28);
	_numMorale = addNumber(px + 76, py + 27, "numMorale");
	_barMorale = new Bar(52, 3, px + 92, py + 28);
	_state->add(_barTimeUnits, "barTUs", "battlescape");
	_state->add(_barEnergy, "barEnergy", "battlescape");
	_state->add(_barHealth, "barHealth", "battlescape");
	_state->add(_barMorale, "barMorale", "battlescape");
	_txtArmor = addText(PanelW - 6, 9, px + 3, py + 35, "weapon", "firstPerson");
	_panelParts = { _txtName, _txtDoom, _txtWeapon, _txtStatus, _numTimeUnits, _barTimeUnits, _numEnergy, _barEnergy,
		_numHealth, _barHealth, _numMorale, _barMorale, _txtArmor };
	for (auto *part : _panelParts)
	{
		_own.insert(part);
		part->setVisible(false);
	}
	_txtDoom->setAlign(ALIGN_RIGHT);
	_txtStatus->setAlign(ALIGN_RIGHT);
	_txtDoom->setText(_state->tr("STR_FPS_DOOM"));
	for (auto *bar : { _barTimeUnits, _barEnergy, _barHealth, _barMorale })
	{
		bar->setScale(1.0);
	}

	// soldier card (laid out under the hovered squad button each frame)
	_card = new Surface(CardW, 40 + PreviewH, 0, CardY);
	_state->add(_card);
	_cardParts.push_back({ _card, 0, 0 });
	_preview = new FirstPersonView(_game, PreviewW, PreviewH, 0, CardY + 36);
	_state->add(_preview);
	_preview->setSavedBattleGame(_save);
	_preview->setMap(_state->_map);
	_preview->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	_preview->shareCache(_view);
	_preview->setParticles(false);
	_cardParts.push_back({ _preview, 4, 36 });
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
	const char *numberIds[4] = { "numTUs", "numEnergy", "numHealth", "numMorale" };
	const char *barIds[4] = { "barTUs", "barEnergy", "barHealth", "barMorale" };
	for (int i = 0; i < 4; ++i)
	{
		const int dx = 4 + (i % 2) * 60, dy = 22 + (i / 2) * 6;
		_cardNumbers[i] = new NumberText(15, 5, 0, 0);
		_state->add(_cardNumbers[i], numberIds[i], "battlescape");
		_cardParts.push_back({ _cardNumbers[i], dx, dy });
		_cardBars[i] = new Bar(40, 3, 0, 0);
		_state->add(_cardBars[i], barIds[i], "battlescape");
		_cardBars[i]->setScale(1.0);
		_cardParts.push_back({ _cardBars[i], dx + 16, dy + 1 });
	}
	for (Text *text : { _cardName, _cardWeapon, _cardStatus, _cardSignal })
	{
		text->setHighContrast(true);
	}
	for (const auto &part : _cardParts)
	{
		part.surface->setVisible(false);
		_own.insert(part.surface);
	}

	_sounds = new FirstPersonSounds();
	makeCanvas();

	// the weapon in hand (placed every frame, bottom right)
	_handSprite = new Surface(32, 48, 0, 0);
	_handSprite->setPalette(_state->getPalette());
	_offSprite = new Surface(32, 48, 0, 0); // (dual wield: the other hand's)
	_offSprite->setPalette(_state->getPalette());
	makeViewModel();

	// what keeps to which corner when the screen is resized (everything else stays top left)
	for (Text *text : _bannerGlow) _anchors[text] = TOP_CENTER;
	_anchors[_txtBanner] = TOP_CENTER;
	_anchors[_txtFallName] = TOP_CENTER;
	_anchors[_crosshair] = CENTER;
	for (Surface *part : std::initializer_list<Surface*>{ _liftUp, _liftDown, _txtLift, _txtLiftKeys }) _anchors[part] = RIGHT_CENTER;
	_anchors[_txtMode] = CENTER;
	_anchors[_txtShot] = CENTER;
	_anchors[_btnEndTurn] = TOP_RIGHT;
	for (Surface *part : _panelParts) _anchors[part] = BOTTOM_LEFT;
	for (Surface *part : { _btnFly, _btnLight, _btnNight, _btnSpecial }) _anchors[part] = TOP_RIGHT;

	layoutTopBar();
	if (lastBattle == _save || (lastAlive && SDL_GetTicks() - lastAlive < 3000))
	{
		_wanted = lastWanted;
		_classicHud = lastClassicHud;
		_mouseLook = !_classicHud;
	}
	remember();
}

/**
 * Notes what the player has chosen, for a remade screen of the same battle (the old screen
 * is only deleted after the new one is made).
 */
void FirstPersonMode::remember() const
{
	lastBattle = _save;
	lastWanted = _wanted;
	lastClassicHud = _classicHud;
}

/**
 * The screen was resized: BattlescapeState moved every surface by (dX / 2, dY) like its
 * bottom-centred HUD. Ours keep to their own corners instead (shadows follow their texts),
 * and the 3D view is made again at the new size.
 */
void FirstPersonMode::resized(int dX, int dY)
{
	// the battlescape moved everything to the middle and the bottom; our overlays live on the canvas
	for (Surface *surface : _own)
	{
		if (surface == _view)
		{
			continue;
		}
		surface->setX(surface->getX() - dX / 2);
		surface->setY(surface->getY() - dY);
	}
	const int oldWidth = (Options::baseXResolution - dX) / _ui, oldHeight = (Options::baseYResolution - dY) / _ui;
	_ui = uiScaleFor(Options::baseXResolution, Options::baseYResolution);
	shiftAnchored(canvasWidth() - oldWidth, canvasHeight() - oldHeight);
	makeCanvas();
	makeViewModel();
	FirstPersonView *view = new FirstPersonView(_game, canvasWidth(), canvasHeight(), 0, 0);
	view->setPalette(_state->getPalette());
	view->setSavedBattleGame(_save);
	view->setMap(_state->_map);
	view->setMaxDistance(_game->getMod()->getFirstPerson().getMaxViewDistance());
	view->setVisible(_view->getVisible());
	std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), (Surface*)_view, (Surface*)view);
	_own.erase(_view);
	_own.insert(view);
	_preview->shareCache(view);
	delete _view;
	_view = view;
	_trackedUnit = nullptr; // re-place the camera
	_warningY += dY; // the HUD's warning place moved with the HUD
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	layoutTopBar();
}

/**
 * How much bigger the overlays are drawn on this screen: about one step per 360 lines of
 * the game's screen, never so big that the canvas is narrower than the original 320.
 */
int FirstPersonMode::uiScaleFor(int width, int height)
{
	return std::max(1, std::min(height / 360, width / 320));
}

int FirstPersonMode::canvasWidth() const
{
	return Options::baseXResolution / _ui;
}

int FirstPersonMode::canvasHeight() const
{
	return Options::baseYResolution / _ui;
}

/**
 * The canvas the overlays are drawn on before being magnified (none needed at scale 1).
 */
void FirstPersonMode::makeCanvas()
{
	delete _canvas;
	_canvas = nullptr;
	if (_ui > 1)
	{
		_canvas = new Surface(canvasWidth(), canvasHeight(), 0, 0);
		std::copy(_state->getPalette(), _state->getPalette() + 256, _canvasPalette);
		_canvas->setPalette(_canvasPalette);
	}
}

/**
 * Moves the overlays for a canvas that grew by dX, dY, each keeping to its corner.
 */
void FirstPersonMode::shiftAnchored(int dX, int dY)
{
	for (Surface *surface : _own)
	{
		if (surface == _view)
		{
			continue;
		}
		auto it = _anchors.find(surface);
		const Anchor anchor = it != _anchors.end() ? it->second : TOP_LEFT;
		surface->setX(surface->getX() + ((anchor == TOP_RIGHT || anchor == RIGHT_CENTER) ? dX : (anchor == TOP_CENTER || anchor == CENTER) ? dX / 2 : 0));
		surface->setY(surface->getY() + (anchor == BOTTOM_LEFT ? dY : (anchor == CENTER || anchor == RIGHT_CENTER) ? dY / 2 : 0));
	}
}

/**
 * Lets a button see a mouse event in canvas coordinates (for its pressed look and click sound).
 */
void FirstPersonMode::handleOnCanvas(TextButton *button, Action *action)
{
	if (_ui == 1)
	{
		button->handle(action, _state);
		return;
	}
	SDL_Event ev = *action->getDetails();
	const double x = action->getAbsoluteXMouse() / _ui, y = action->getAbsoluteYMouse() / _ui;
	ev.button.x = (Uint16)std::lround(x * action->getXScale() + action->getLeftBlackBand());
	ev.button.y = (Uint16)std::lround(y * action->getYScale() + action->getTopBlackBand());
	Action scaled(&ev, action->getXScale(), action->getYScale(), action->getTopBlackBand(), action->getLeftBlackBand());
	button->handle(&scaled, _state);
}

/**
 * Where the game's warnings go in 3D: just above the (magnified) info panel.
 */
int FirstPersonMode::warningY() const
{
	return Options::baseYResolution - PanelH * _ui - 28;
}

/**
 * Adds a small button in the style of the original UI (colours from the mod's "firstPerson" interface).
 */
TextButton *FirstPersonMode::makeButton(int x, int w, const std::string &label)
{
	TextButton *btn = new TextButton(w, BarH, x, BarY);
	_state->add(btn, "button", "firstPerson");
	btn->setSmall();
	btn->setText(label);
	_own.insert(btn);
	return btn;
}

/**
 * How wide a button must be for its label in the small font.
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
 */
Text *FirstPersonMode::addText(int w, int h, int x, int y, const std::string &id, const std::string &category)
{
	Text *shadow = new Text(w, h, x + 1, y + 1);
	Text *text = new Text(w, h, x, y);
	_state->add(shadow);
	_state->add(text, id, category);
	shadow->setColor(10); // glyph shades 1..5 land on 11..15: dark greys, inside the group
	text->setHighContrast(true);
	_shadows.push_back(shadow);
	_own.insert(shadow);
	_own.insert(text);
	return text;
}

/**
 * Adds a number (like the original HUD's stats) with a dark shadow.
 */
NumberText *FirstPersonMode::addNumber(int x, int y, const std::string &id)
{
	NumberText *shadow = new NumberText(15, 5, x + 1, y + 1);
	NumberText *number = new NumberText(15, 5, x, y);
	_state->add(shadow);
	_state->add(number, id, "battlescape");
	shadow->setColor(10);
	_numberShadows.push_back(shadow);
	_own.insert(shadow);
	_own.insert(number);
	return number;
}

/**
 * Copies text, alignment and visibility of every text onto its shadow.
 */
void FirstPersonMode::syncShadows()
{
	// texts and their shadows were added in pairs, the shadow right before the text
	auto &surfaces = _state->_surfaces;
	for (size_t i = 0; i + 1 < surfaces.size(); ++i)
	{
		if (Text *shadow = dynamic_cast<Text*>(surfaces[i]))
		{
			if (std::find(_shadows.begin(), _shadows.end(), shadow) == _shadows.end()) continue;
			Text *text = static_cast<Text*>(surfaces[i + 1]);
			shadow->setX(text->getX() + 1);
			shadow->setY(text->getY() + 1);
			shadow->setVisible(text->getVisible());
			if (shadow->getText() != text->getText())
			{
				shadow->setAlign(text->getAlign());
				shadow->setText(text->getText());
			}
		}
		else if (NumberText *numberShadow = dynamic_cast<NumberText*>(surfaces[i]))
		{
			if (std::find(_numberShadows.begin(), _numberShadows.end(), numberShadow) == _numberShadows.end()) continue;
			NumberText *number = static_cast<NumberText*>(surfaces[i + 1]);
			numberShadow->setX(number->getX() + 1);
			numberShadow->setY(number->getY() + 1);
			numberShadow->setVisible(number->getVisible());
			numberShadow->setValue(number->getValue());
		}
	}
}

/**
 * Cleans up. The surfaces were added to the state, which deletes them.
 */
FirstPersonMode::~FirstPersonMode()
{
	_state->_map->setSoundListener(nullptr);
	_state->_map->setNightVisionHoldBlocked(false);
	// a quickload deletes the battle before this screen goes; only a battle that lives on (the
	// screen remade after an options change) gets its pathfinding back to the rules
	SavedGame *game = _game->getSavedGame();
	if (game && game->getSavedBattle() == _save)
	{
		_save->getPathfinding()->setVault(nullptr, {});
	}
	delete _handSprite; // not added to the state
	delete _offSprite;
	delete _sounds;
	delete _canvas;
}

bool FirstPersonMode::isActive() const
{
	return _view->getVisible();
}

void FirstPersonMode::toggleView()
{
	_wanted = !_wanted;
	remember();
	think();
}

// ---------------------------------------------------------------------------------------
// view switching and mouse modes
// ---------------------------------------------------------------------------------------

/**
 * Shows or hides the 3D view in place of the isometric map.
 */
void FirstPersonMode::setActive(bool active)
{
	if (!active)
	{
		_save->getPathfinding()->setFlightCost(nullptr, -1); // by the rules on the isometric map
		endVault();
	}
	// in 3D the night vision hold key (Space) jumps; night vision there is gear with its own key
	_state->_map->setNightVisionHoldBlocked(active && Options::keyFpsJump == Options::keyNightVisionHold);
	// in 3D, sounds are panned for the soldier's ears (where the view looks), not the map's camera
	if (active)
	{
		_state->_map->setSoundListener([this](const Position &tile) { return _view->soundAngle(tile); });
	}
	else
	{
		_state->_map->setSoundListener(nullptr);
	}
	_view->setVisible(active);
	_state->_map->setVisible(!active);
	_state->_map->invalidate();
	_dragLook = false;
	_mouseLook = active && !_classicHud;
	_trackedUnit = nullptr; // re-place the camera on the unit
	// keep warnings ("not enough time units"...) visible above the panel when the classic HUD is hidden
	_state->_warning->setY(active && !_classicHud ? warningY() : _warningY);
}

/**
 * Shows or hides the original bottom HUD over the 3D view. Showing it frees the mouse
 * so its buttons (fire modes, kneel, ...) can be clicked; hiding it goes back to mouse look.
 */
void FirstPersonMode::setClassicHud(bool on)
{
	_classicHud = on;
	remember();
	_dragLook = false;
	_mouseLook = !on;
	_state->_warning->setY(!on ? warningY() : _warningY);
}

void FirstPersonMode::setMouseLook(bool on)
{
	_mouseLook = on;
	_dragLook = false;
}

bool FirstPersonMode::wantsMouseCapture() const
{
	return isActive() && (_mouseLook || _dragLook);
}

/**
 * Turns the view by a relative mouse movement.
 */
void FirstPersonMode::look(int dx, int dy)
{
	dx = std::clamp(dx, -100, 100);
	dy = std::clamp(dy, -100, 100);
	const double degreesPerPixel = std::clamp(Options::fpsMouseSensitivity, 1, 20) * 0.05;
	_view->rotate(dx * degreesPerPixel, (Options::fpsInvertMouse ? dy : -dy) * degreesPerPixel);
}

/**
 * The scroll keys (arrows by default) turn the view; the isometric map isn't listening while hidden.
 */
void FirstPersonMode::turnWithKeys(double dt)
{
	const Uint8 *keys = SDL_GetKeyState(nullptr);
	const double turnSpeed = 90.0; // degrees per second
	double yaw = 0.0, pitch = 0.0;
	if (keys[Options::keyBattleLeft]) yaw -= turnSpeed * dt;
	if (keys[Options::keyBattleRight]) yaw += turnSpeed * dt;
	if (keys[Options::keyBattleUp]) pitch += turnSpeed * dt;
	if (keys[Options::keyBattleDown]) pitch -= turnSpeed * dt;
	if (yaw != 0.0 || pitch != 0.0)
	{
		_view->rotate(yaw, pitch);
	}
}

/**
 * Looking around turns the soldier for free: the unit faces the octant the camera looks at,
 * and its field of view is updated like after a normal turn (minus the TU cost).
 */
void FirstPersonMode::faceLookDirection(BattleUnit *unit)
{
	if (!canCommit() || unit->getStatus() != STATUS_STANDING || !_state->playableUnitSelected())
	{
		return;
	}
	// yaw 0 = east; unit direction 0 = north, clockwise in 45 degree steps
	const int direction = (((int)std::lround((_view->getYawDegrees() + 90.0) / 45.0)) % 8 + 8) % 8;
	if (direction == unit->getDirection())
	{
		return;
	}
	unit->setDirection(direction);
	TileEngine *tileEngine = _save->getTileEngine();
	tileEngine->calculateFOV(unit);
	tileEngine->calculateFOV(unit->getPosition(), 1, false); // others' view of this unit, like UnitTurnBState
	_state->updateSoldierInfo(false);
}

/**
 * Turns the view towards the n-th visible enemy (the red indicators of the classic HUD).
 */
void FirstPersonMode::lookAtEnemy(int index)
{
	if (index < 0 || index >= BattlescapeState::VISIBLE_MAX)
	{
		return;
	}
	BattleUnit *enemy = _state->_visibleUnit[index];
	if (!enemy || enemy->isOut())
	{
		return;
	}
	const Position pos = enemy->getPosition();
	const Tile *tile = _save->getTile(pos);
	const int size = enemy->getArmor()->getSize();
	const double target[3] = {
		pos.x * 16.0 + 8.0 * size,
		pos.y * 16.0 + 8.0 * size,
		pos.z * 24.0 - (tile ? tile->getTerrainLevel() : 0) + enemy->getFloatHeight() + enemy->getHeight() * 0.6 };
	_view->lookAt(target);
}

// ---------------------------------------------------------------------------------------
// camera and movement
// ---------------------------------------------------------------------------------------

/**
 * Puts the camera in the middle of the unit's tile (or 2x2 footprint), at eye height.
 */
void FirstPersonMode::snapToTile(BattleUnit *unit)
{
	const Position tile = unit->getPosition();
	const int size = unit->getArmor()->getSize();
	_posX = tile.x * 16.0 + 8.0 * size;
	_posY = tile.y * 16.0 + 8.0 * size;
	_eyeZ = eyeHeight(unit);
	_trackedTile = tile;
	_stepPending = false;
	_throughDoor = false;
	_velX = _velY = 0.0;
	_rideStart = 0;
	_view->setHiddenFloor(-1);
}

/**
 * Keeps the camera on a unit: follows steps it walks, re-places it when the unit changes or
 * moves some other way, moves it with the keys, and eases the eye height.
 */
void FirstPersonMode::updateCamera(BattleUnit *unit, bool canMove, double dt)
{
	const Position tile = unit->getPosition();
	if (unit != _trackedUnit)
	{
		_trackedUnit = unit;
		_view->setCameraUnit(unit);
		_view->lookAlong(unit->getDirection());
		endVault();
		snapToTile(unit);
	}
	else if (_stepPending)
	{
		if (!_state->_battleGame->isBusy())
		{
			_stepPending = false;
			_save->getPathfinding()->setVault(nullptr, {});
			if (_fallFromZ >= 0 && unit->haveNoFloorBelow() && unit->getMovementType() != MT_FLY && !unit->isOut())
			{
				// stepped off a ledge into the air: the game's own fall takes the soldier down
				_save->addFallingUnit(unit);
				_state->_battleGame->statePushFront(new UnitFallBState(_state->_battleGame));
				_stepPending = true; // wait for the landing
				_trackedTile = tile;
				return updateCamera(unit, canMove, dt);
			}
			if (_fallFromZ >= 0)
			{
				// landed after stepping off a ledge
				const int levels = _fallFromZ - tile.z;
				_fallFromZ = -1;
				if (levels > 0) fallDamage(unit, levels);
			}
			if (_vaulting && (tile.x != (int)std::floor(_vaultTo[0] / 16.0) || tile.y != (int)std::floor(_vaultTo[1] / 16.0)))
			{
				// the vault was cut short (reaction fire, out of energy, ...): back with the soldier
				endVault();
				snapToTile(unit);
			}
			if (tile.x == _stepFrom.x && tile.y == _stepFrom.y)
			{
				// the step didn't happen (out of TUs, unseen unit in the way, ...): back into the old tile
				const int offset = 8 * (unit->getArmor()->getSize() - 1);
				_posX = clampAxis(tile.x, _posX, offset);
				_posY = clampAxis(tile.y, _posY, offset);
			}
			_trackedTile = tile;
		}
	}
	else if (tile.x == _trackedTile.x && tile.y == _trackedTile.y && tile.z != _trackedTile.z)
	{
		// up or down a level where it stands: a lift ride is already gliding (see below); a fall
		// or a landing glides too, through the upper level's floor (left out for it)
		if (!_rideStart)
		{
			_view->setHiddenFloor(_save->getTileIndex(Position(tile.x, tile.y, std::max(tile.z, _trackedTile.z))));
			_rideStart = SDL_GetTicks();
			_rideTime = DropTime;
			_rideFrom = _eyeZ;
			_rideTo = _eyeZ + (tile.z - _trackedTile.z) * 24.0;
			_rideLevel = _trackedTile.z;
		}
		_trackedTile = tile;
	}
	else if (tile != _trackedTile)
	{
		snapToTile(unit); // moved some other way: an order on the isometric map, falling, ...
	}
	// a lift ride (or flying up or down) glides the eye from the moment it starts, slowly; the
	// game moves the unit up half way through its own short move, the eye takes its time
	const int vertical = unit->getVerticalDirection();
	if (vertical && !_rideVertical)
	{
		const int dir = vertical == Pathfinding::DIR_UP ? 1 : -1;
		const Position at = unit->getPosition();
		_view->setHiddenFloor(_save->getTileIndex(Position(at.x, at.y, at.z + (dir > 0 ? 1 : 0))));
		_rideStart = SDL_GetTicks();
		_rideTime = RideTime;
		_rideFrom = _eyeZ;
		_rideTo = _eyeZ + dir * 24.0;
		_rideLevel = at.z;
	}
	_rideVertical = vertical != 0;

	std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);
	move(unit, canMove, dt);

	const double eyeTarget = eyeHeight(unit);
	const double ride = _rideStart ? (SDL_GetTicks() - _rideStart) / (double)std::max<Uint32>(1, _rideTime) : 1.0;
	if (ride < 1.0)
	{
		// towards the new level's eye height once the unit is there, the estimate until then
		const double goal = unit->getPosition().z != _rideLevel ? eyeTarget : _rideTo;
		_eyeZ = _rideFrom + (goal - _rideFrom) * ride * ride * (3.0 - 2.0 * ride); // eased in and out
	}
	else
	{
		if (_rideStart) _view->setHiddenFloor(-1);
		_rideStart = 0;
		_eyeZ += (eyeTarget - _eyeZ) * std::min(1.0, dt * 12.0);
	}
	// head bob: a gentle dip with every step (footsteps fall every 8 voxels), fading in and out
	const bool walking = std::abs(_velX) + std::abs(_velY) > 4.0;
	_bobAmount += ((walking && Options::fpsHeadBob ? 1.0 : 0.0) - _bobAmount) * std::min(1.0, dt * 8.0);
	const double bob = (std::cos(_bobPhase * 2.0 * Pi / 8.0) - 1.0) * 0.3 * _bobAmount;
	// a melee swing lunges the view forward a little, and the blow lands with a jolt
	double windup = 0.0, strike = 0.0, lunge = 0.0;
	if (swingPhase(windup, strike))
	{
		lunge = 2.0 * strike - 1.0 * windup;
		if (strike > 0.9 && !_swingJolted)
		{
			_swingJolted = true;
			_view->addShake(1.4f);
		}
	}
	const double yaw = _view->getYawDegrees() * Pi / 180.0;
	_view->setEye(_posX + std::cos(yaw) * lunge, _posY + std::sin(yaw) * lunge, _eyeZ + bob + jumpLift(unit));
}

/**
 * Where the 3D camera's eye is: the game's sight origin, lowered a little (setting "Eye height"):
 * X-COM soldiers fill a level almost to the ceiling, so the view sits a bit lower to give rooms
 * some headroom (the game's own line of sight stays where it is).
 */
double FirstPersonMode::eyeHeight(const BattleUnit *unit) const
{
	const double lower = std::clamp(Options::fpsEyeLower, 0, 6) * (unit->isKneeled() ? 0.5 : 1.0);
	return _save->getTileEngine()->getSightOriginVoxel(const_cast<BattleUnit*>(unit)).z + 0.5 - lower;
}

/**
 * Keeps a coordinate inside a tile, away from its edges.
 */
double FirstPersonMode::clampAxis(int tile, double value, int offset) const
{
	return std::clamp(value, tile * 16.0 + EdgeMargin + offset, tile * 16.0 + 16.0 - EdgeMargin + offset);
}

/**
 * Could the unit step from the camera's tile in this direction now, by the original rules
 * (walls, objects, corners, stairs, TU, energy, reserved TU)?
 */
bool FirstPersonMode::stepAllowed(BattleUnit *unit, int dx, int dy)
{
	const int dir = Pathfinding::vectorToDirection(Position(dx, dy, 0));
	if (dir < 0 || dir > 7)
	{
		return false;
	}
	if (_stepAllowed[dir] < 0)
	{
		const Position start(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
		const PathfindingStep step = _save->getPathfinding()->getTUCost(start, dir, unit, nullptr, BAM_NORMAL);
		bool ok = step.cost.time < Pathfinding::INVALID_MOVE_COST;
		if (ok && ((canFly(unit) && !flightOn(unit)) || !Options::fpsFalling))
		{
			// flight off (or falling switched off): don't step off a ledge into the air (a ramp
			// or stairs down is ground)
			const Tile *to = _save->getTile(start + Position(dx, dy, 0));
			const Tile *below = to ? _save->getBelowTile(to) : nullptr;
			ok = !to || !to->hasNoFloor(_save) || (below && !below->hasNoFloor(_save) && below->getTerrainLevel() <= -12);
		}
		if (ok && !Options::fpsDoomMode)
		{
			ok = unit->getTimeUnits() >= step.cost.time && unit->getEnergy() >= step.cost.energy
				&& _state->_battleGame->checkReservedTU(unit, step.cost.time, step.cost.energy, true);
		}
		_stepAllowed[dir] = ok ? 1 : 0;
	}
	return _stepAllowed[dir] == 1;
}

/**
 * Can the player act right now (their turn, nothing else going on)?
 */
bool FirstPersonMode::canCommit() const
{
	return _state->allowButtons() && _state->_popups.empty();
}

/**
 * Commits one step of the unit into the neighbouring tile, through the normal walking
 * state, so every original rule applies: TU and energy, doors, stairs, falling, mines,
 * spotting and reaction fire.
 * @return False if the step couldn't be started.
 */
bool FirstPersonMode::commitStep(BattleUnit *unit, int dx, int dy)
{
	const Position from(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
	const Position target(from.x + dx, from.y + dy, from.z);
	Pathfinding *pathfinding = _save->getPathfinding();
	pathfinding->calculate(unit, target, BAM_NORMAL);
	// the direct step; or, over a ledge, the step and the fall straight down after it
	const std::vector<int> &path = pathfinding->getPath();
	const int dir = Pathfinding::vectorToDirection(Position(dx, dy, 0));
	// over a ledge the search plans nothing (it won't step into the air): the step is set by hand,
	// and the game's own falling takes over once the soldier is out there (setting "Falling")
	const Tile *ahead = _save->getTile(target);
	const bool fall = path.empty() && Options::fpsFalling && ahead && ahead->hasNoFloor(_save) && !(canFly(unit) && flightOn(unit));
	if (fall)
	{
		pathfinding->setPath(unit, { dir });
	}
	else if (path.size() != 1)
	{
		// the direct step is valid, but if pathfinding prefers a detour we don't take it
		pathfinding->abortPath();
		return false;
	}
	_fallFromZ = fall ? from.z : -1;
	BattleAction walk;
	walk.type = BA_WALK;
	walk.actor = unit;
	walk.target = target;
	walk.instantMove = true; // the 3D view moves smoothly on its own
	_state->_battleGame->statePushBack(new UnitWalkBState(_state->_battleGame, walk));
	_stepPending = true;
	_stepFrom = from;
	_trackedTile = Position(target.x, target.y, from.z);
	std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);
	if (canFly(unit) && !flightOn(unit))
	{
		// a flying suit doesn't take ramps down, it floats off the edge: with flight off it lands
		const Tile *to = _save->getTile(target);
		if (to && to->hasNoFloor(_save))
		{
			_flight[unit] = false;
			_landAfterStep = true;
		}
	}
	return true;
}

/**
 * Is there a closed door between the camera's tile and its neighbour in this direction
 * (a straight step)? Opened normal doors turn into their open version, which isn't a door.
 */
bool FirstPersonMode::closedDoor(int dx, int dy, int z) const
{
	if (dx != 0 && dy != 0)
	{
		return false;
	}
	// a tile owns its west and north walls
	const Position owner(_trackedTile.x + std::max(dx, 0), _trackedTile.y + std::max(dy, 0), z);
	const Tile *tile = _save->getTile(owner);
	if (!tile)
	{
		return false;
	}
	const TilePart part = dx != 0 ? O_WESTWALL : O_NORTHWALL;
	const MapData *md = tile->getMapData(part);
	return md && (md->isDoor() || (md->isUFODoor() && !tile->isUfoDoorOpen(part)));
}

/**
 * A step through a door: the camera waits in front of it while the door swings or slides
 * open, then walks through the doorway on its own and hands back to the keys.
 * @return False when there's no door step going on.
 */
bool FirstPersonMode::walkThroughDoor(BattleUnit *unit, double &nx, double &ny, double dt)
{
	if (!_throughDoor)
	{
		return false;
	}
	const Position pos = unit->getPosition();
	const bool stillHere = pos.x == _stepFrom.x && pos.y == _stepFrom.y;
	const int offset = 8 * (unit->getArmor()->getSize() - 1);
	// the wall the door is in (a tile owns its west and north walls)
	const Position wall(_stepFrom.x + std::max(_doorStep[0], 0), _stepFrom.y + std::max(_doorStep[1], 0), _stepFrom.z);
	const int wallPart = _doorStep[0] != 0 ? O_WESTWALL : O_NORTHWALL;
	const bool waiting = SDL_GetTicks() - _doorStepStart < DoorWait || _view->isDoorMoving(wall, wallPart);
	if ((_stepPending && stillHere) || (waiting && SDL_GetTicks() - _doorStepStart < 3000))
	{
		nx = clampAxis(_stepFrom.x, _posX, offset);
		ny = clampAxis(_stepFrom.y, _posY, offset);
		return true;
	}
	const int cameraX = (int)std::floor((_posX - offset) / 16.0), cameraY = (int)std::floor((_posY - offset) / 16.0);
	const bool inside = (_doorStep[0] == 0 || (_posX - offset - _trackedTile.x * 16.0 >= EdgeMargin && _trackedTile.x * 16.0 + 16.0 - (_posX - offset) >= EdgeMargin))
		&& (_doorStep[1] == 0 || (_posY - offset - _trackedTile.y * 16.0 >= EdgeMargin && _trackedTile.y * 16.0 + 16.0 - (_posY - offset) >= EdgeMargin));
	if (!stillHere && (cameraX != _trackedTile.x || cameraY != _trackedTile.y || !inside))
	{
		// through the doorway, straight on
		const double distance = std::clamp(Options::fpsWalkSpeed, 1, 8) * 16.0 * dt;
		nx = _posX + _doorStep[0] * distance;
		ny = _posY + _doorStep[1] * distance;
		if (_doorStep[0] == 0) nx = clampAxis(_trackedTile.x, nx, offset);
		if (_doorStep[1] == 0) ny = clampAxis(_trackedTile.y, ny, offset);
		return true;
	}
	if (!_stepPending)
	{
		_throughDoor = false; // through, or the step didn't happen
	}
	return false;
}

/**
 * Jumps: a free hop of the camera. Walking into something low (a fence, a table, a crate),
 * it's a vault over it instead (option "Jump over low obstacles").
 */
void FirstPersonMode::jump(BattleUnit *unit)
{
	if (_jumpStart || !unit || unit->isOut() || unit->isKneeled() || unit->getStatus() != STATUS_STANDING || (canFly(unit) && flightOn(unit)))
	{
		return; // in the air already, or down (flying soldiers rise with the up key)
	}
	const Uint8 *keys = SDL_GetKeyState(nullptr);
	const double forward = (keys[Options::keyFpsForward] ? 1.0 : 0.0) - (keys[Options::keyFpsBack] ? 1.0 : 0.0);
	const double strafe = (keys[Options::keyFpsStrafeRight] ? 1.0 : 0.0) - (keys[Options::keyFpsStrafeLeft] ? 1.0 : 0.0);
	if ((forward != 0.0 || strafe != 0.0) && !_stepPending && canCommit() && _state->playableUnitSelected())
	{
		// the way the keys push, straight along the grid
		const double yaw = _view->getYawDegrees() * Pi / 180.0;
		const double mx = std::cos(yaw) * forward - std::sin(yaw) * strafe, my = std::sin(yaw) * forward + std::cos(yaw) * strafe;
		const int dx = std::abs(mx) >= std::abs(my) ? (mx > 0.0 ? 1 : -1) : 0;
		const int dy = std::abs(mx) >= std::abs(my) ? 0 : (my > 0.0 ? 1 : -1);
		std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);
		if (!stepAllowed(unit, dx, dy) && tryVault(unit, dx, dy))
		{
			return;
		}
	}
	_jumpStart = SDL_GetTicks();
	_jumpTime = JumpTime;
	_jumpHeight = JumpHeight;
}

/**
 * Something low enough to jump over: a wall or an object that blocks walking, no higher than
 * about a metre, not a door, standing on the floor.
 */
bool FirstPersonMode::lowObstacle(const Tile *tile, int part) const
{
	const MapData *data = tile ? tile->getMapData((TilePart)part) : nullptr;
	if (!data || data->isDoor() || data->isUFODoor() || data->getTerrainLevel() != 0 || data->getBigWall() > 1)
	{
		return false;
	}
	int top = 0;
	for (int layer = 0; layer < 12; ++layer)
	{
		if (data->getLoftID(layer)) top = (layer + 1) * 2;
	}
	return top > 0 && top <= VaultMaxHeight;
}

/**
 * Vaults over a low obstacle straight ahead: a low wall onto the next tile, or something low
 * on the next tile onto the one beyond. The step is walked by the rules (the pathfinding lets
 * the obstacle through): the jump is free, the walk costs its normal time units.
 * @return False if there's nothing to vault, or the soldier can't.
 */
bool FirstPersonMode::tryVault(BattleUnit *unit, int dx, int dy)
{
	if (!Options::fpsVault || unit->getArmor()->getSize() != 1)
	{
		return false;
	}
	const Position from(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
	const Position next = from + Position(dx, dy, 0);
	const TilePart wallPart = dx != 0 ? O_WESTWALL : O_NORTHWALL;
	auto wallBetween = [&](const Position &a, const Position &b) { return _save->getTile(dx + dy > 0 ? b : a); }; // a tile owns its west and north walls
	auto blocking = [](const Tile *tile, TilePart part) {
		return tile->getMapData(part) && tile->getTUCost(part, MT_WALK) >= Pathfinding::INVALID_MOVE_COST;
	};
	const Tile *nextTile = _save->getTile(next);
	const Tile *wallTile = wallBetween(from, next);
	if (!nextTile || !wallTile)
	{
		return false;
	}
	std::vector<std::pair<const Tile*, int>> parts;
	if (blocking(wallTile, wallPart))
	{
		if (!lowObstacle(wallTile, wallPart)) return false;
		parts.push_back({ wallTile, wallPart });
	}
	Position landing = next;
	if (blocking(nextTile, O_OBJECT))
	{
		if (!lowObstacle(nextTile, O_OBJECT)) return false;
		parts.push_back({ nextTile, O_OBJECT });
		landing = next + Position(dx, dy, 0);
		const Tile *beyond = _save->getTile(landing), *wall2 = wallBetween(next, landing);
		if (!beyond || !wall2)
		{
			return false;
		}
		if (blocking(wall2, wallPart))
		{
			if (!lowObstacle(wall2, wallPart)) return false;
			parts.push_back({ wall2, wallPart });
		}
	}
	if (parts.empty())
	{
		return false; // nothing low in the way: whatever stops the step isn't for jumping
	}
	Pathfinding *pathfinding = _save->getPathfinding();
	pathfinding->setVault(unit, parts);
	pathfinding->calculate(unit, landing, BAM_NORMAL);
	const int dir = Pathfinding::vectorToDirection(Position(dx, dy, 0));
	const std::vector<int> &path = pathfinding->getPath();
	bool ok = path.size() == (landing == next ? 1u : 2u) && std::all_of(path.begin(), path.end(), [dir](int d) { return d == dir; });
	if (ok && !Options::fpsDoomMode)
	{
		const int cost = pathfinding->getTotalTUCost();
		ok = unit->getTimeUnits() >= cost && _state->_battleGame->checkReservedTU(unit, cost, 0, true);
		if (!ok)
		{
			_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		}
	}
	if (!ok)
	{
		pathfinding->abortPath();
		pathfinding->setVault(nullptr, {});
		return false;
	}
	BattleAction walk;
	walk.type = BA_WALK;
	walk.actor = unit;
	walk.target = landing;
	walk.instantMove = true;
	_state->_battleGame->statePushBack(new UnitWalkBState(_state->_battleGame, walk));
	_stepPending = true;
	_stepFrom = from;
	_trackedTile = Position(landing.x, landing.y, from.z);
	std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);
	_vaulting = true;
	_vaultFrom[0] = _posX;
	_vaultFrom[1] = _posY;
	_vaultTo[0] = landing.x * 16.0 + 8.0;
	_vaultTo[1] = landing.y * 16.0 + 8.0;
	_jumpStart = SDL_GetTicks();
	_jumpTime = landing == next ? 450 : 650;
	_jumpHeight = VaultHeight;
	return true;
}

/**
 * During a vault the camera flies over the obstacle to the middle of the landing tile.
 * @return True while it does (the movement keys wait).
 */
bool FirstPersonMode::vaultGlide(BattleUnit *, double &nx, double &ny)
{
	if (!_vaulting)
	{
		return false;
	}
	const double t = _jumpStart ? std::min(1.0, (SDL_GetTicks() - _jumpStart) / (double)_jumpTime) : 1.0;
	nx = _vaultFrom[0] + (_vaultTo[0] - _vaultFrom[0]) * t;
	ny = _vaultFrom[1] + (_vaultTo[1] - _vaultFrom[1]) * t;
	_velX = _velY = 0.0;
	if (t >= 1.0 && !_stepPending)
	{
		_vaulting = false;
	}
	return true;
}

/**
 * How high the jump has lifted the eye just now (an arc); landing ends it with a footfall.
 */
double FirstPersonMode::jumpLift(BattleUnit *unit)
{
	if (!_jumpStart)
	{
		return 0.0;
	}
	const double t = (SDL_GetTicks() - _jumpStart) / (double)std::max<Uint32>(1, _jumpTime);
	if (t >= 1.0)
	{
		_jumpStart = 0;
		footsteps(unit, 8.0); // the landing
		return 0.0;
	}
	// the head stops short of anything above it (a ceiling, a wing, a ledge)
	const double lift = _jumpHeight * 4.0 * t * (1.0 - t);
	const int maxZ = _save->getMapSizeZ() * 24 - 1;
	for (int up = 0; up <= (int)std::ceil(lift); ++up)
	{
		const int z = (int)_eyeZ + up + 2; // the top of the head
		if (z > maxZ) break;
		if (_save->getTileEngine()->voxelCheck(Position((int)_posX, (int)_posY, z), nullptr, true) != V_EMPTY)
		{
			return std::min(lift, std::max(0.0, up - 1.0));
		}
	}
	return lift;
}

/**
 * Stops a vault: the pathfinding goes back to the rules, the camera to the keys.
 */
void FirstPersonMode::endVault()
{
	_save->getPathfinding()->setVault(nullptr, {});
	_vaulting = false;
	_jumpStart = 0;
}

/**
 * Night vision in 3D comes with gear (option "Night vision needs gear"): an armor or item the
 * mod lists, or eyes that see as far in the dark as by day (aliens).
 */
bool FirstPersonMode::hasNightVision(const BattleUnit *unit) const
{
	if (!unit)
	{
		return false;
	}
	if (!Options::fpsNightVisionGear || unit->getMaxViewDistanceAtDark(nullptr) >= unit->getMaxViewDistanceAtDay(nullptr))
	{
		return true;
	}
	const RuleFirstPerson &rules = _game->getMod()->getFirstPerson();
	if (rules.givesNightVision(unit->getArmor()->getType()))
	{
		return true;
	}
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
 * Night vision on/off (the NIGHT VISION button and its key), for a soldier with the gear.
 */
void FirstPersonMode::toggleNightVision(BattleUnit *unit, Action *action)
{
	if (!hasNightVision(unit))
	{
		_state->warning("STR_FPS_NO_NIGHT_VISION");
		return;
	}
	_state->btnNightVisionClick(action);
}

/**
 * Real-time movement with the movement keys. Inside a tile the camera moves freely;
 * crossing into the next tile commits a step (diagonal first, then sliding along either
 * axis); where no step is possible the camera stops short of the edge.
 */
void FirstPersonMode::move(BattleUnit *unit, bool canMove, double dt)
{
	const Uint8 *keys = SDL_GetKeyState(nullptr);
	double forward = 0.0, strafe = 0.0;
	if (_movementHeld && !keys[Options::keyFpsForward] && !keys[Options::keyFpsBack]
		&& !keys[Options::keyFpsStrafeLeft] && !keys[Options::keyFpsStrafeRight])
	{
		_movementHeld = false;
	}
	if (canMove && !_movementHeld)
	{
		forward = (keys[Options::keyFpsForward] ? 1.0 : 0.0) - (keys[Options::keyFpsBack] ? 1.0 : 0.0);
		strafe = (keys[Options::keyFpsStrafeRight] ? 1.0 : 0.0) - (keys[Options::keyFpsStrafeLeft] ? 1.0 : 0.0);
	}
	const int offset = 8 * (unit->getArmor()->getSize() - 1);
	double nx = _posX, ny = _posY;
	const double speed = std::clamp(Options::fpsWalkSpeed, 1, 8) * 16.0;
	if (vaultGlide(unit, nx, ny))
	{
		_posX = nx;
		_posY = ny;
		return;
	}
	if (walkThroughDoor(unit, nx, ny, dt))
	{
		const double moved = std::sqrt((nx - _posX) * (nx - _posX) + (ny - _posY) * (ny - _posY));
		footsteps(unit, moved);
		_velX = dt > 0.0 ? (nx - _posX) / dt : 0.0; // carry on smoothly when the doorway is behind
		_velY = dt > 0.0 ? (ny - _posY) / dt : 0.0;
		_posX = nx;
		_posY = ny;
		return;
	}
	// a little momentum: the soldier speeds up and slows down over a fraction of a second
	double wantX = 0.0, wantY = 0.0;
	if (forward != 0.0 || strafe != 0.0)
	{
		const double yaw = _view->getYawDegrees() * Pi / 180.0;
		const double mx = std::cos(yaw) * forward - std::sin(yaw) * strafe;
		const double my = std::sin(yaw) * forward + std::cos(yaw) * strafe;
		const double len = std::sqrt(mx * mx + my * my);
		wantX = mx / len * speed;
		wantY = my / len * speed;
	}
	const double blend = std::min(1.0, dt * 12.0);
	_velX += (wantX - _velX) * blend;
	_velY += (wantY - _velY) * blend;
	if (wantX == 0.0 && std::abs(_velX) < 1.0) _velX = 0.0;
	if (wantY == 0.0 && std::abs(_velY) < 1.0) _velY = 0.0;
	nx += _velX * dt;
	ny += _velY * dt;
	// only a held key walks into the next tile (and pays for it); gliding to a stop doesn't
	const bool driving = wantX != 0.0 || wantY != 0.0;

	// a closed door ahead: stop in front of it, and walking on opens it
	if (unit->getArmor()->getSize() == 1)
	{
		const double lowX = _trackedTile.x * 16.0, lowY = _trackedTile.y * 16.0;
		const int z = unit->getPosition().z;
		const int sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (const auto &side : sides)
		{
			// moving towards that side, and close to it
			const bool pushing = side[0] > 0 ? nx > _posX && nx > lowX + 16.0 - DoorMargin
				: side[0] < 0 ? nx < _posX && nx < lowX + DoorMargin
				: side[1] > 0 ? ny > _posY && ny > lowY + 16.0 - DoorMargin
				: ny < _posY && ny < lowY + DoorMargin;
			if (!pushing || !closedDoor(side[0], side[1], z))
			{
				continue;
			}
			if (side[0] > 0) nx = std::min(nx, lowX + 16.0 - DoorMargin);
			if (side[0] < 0) nx = std::max(nx, lowX + DoorMargin);
			if (side[1] > 0) ny = std::min(ny, lowY + 16.0 - DoorMargin);
			if (side[1] < 0) ny = std::max(ny, lowY + DoorMargin);
			if (!_stepPending && driving && canMove && canCommit() && unit->getStatus() == STATUS_STANDING && stepAllowed(unit, side[0], side[1])
				&& commitStep(unit, side[0], side[1]))
			{
				_throughDoor = true;
				_doorStep[0] = side[0];
				_doorStep[1] = side[1];
				_doorStepStart = SDL_GetTicks();
				footsteps(unit, std::sqrt((nx - _posX) * (nx - _posX) + (ny - _posY) * (ny - _posY)));
				_posX = nx;
				_posY = ny;
				return;
			}
		}
	}

	const int cx = std::clamp((int)std::floor((nx - offset) / 16.0) - _trackedTile.x, -1, 1);
	const int cy = std::clamp((int)std::floor((ny - offset) / 16.0) - _trackedTile.y, -1, 1);
	if (cx != 0 || cy != 0)
	{
		bool moved = false;
		const int candidates[3][2] = { { cx, cy }, { cx, 0 }, { 0, cy } };
		for (int i = 0; i < 3 && !moved; ++i)
		{
			const int sx = candidates[i][0], sy = candidates[i][1];
			if ((sx == 0 && sy == 0) || (i > 0 && (cx == 0 || cy == 0)))
			{
				continue;
			}
			if (driving && canMove && canCommit() && unit->getStatus() == STATUS_STANDING && stepAllowed(unit, sx, sy))
			{
				const Position tile = _trackedTile;
				if (commitStep(unit, sx, sy))
				{
					// keep moving along the committed axes, stay inside the old tile on the other
					if (sx == 0) nx = clampAxis(tile.x, nx, offset);
					if (sy == 0) ny = clampAxis(tile.y, ny, offset);
					moved = true;
				}
			}
		}
		if (!moved)
		{
			if (cx != 0) nx = clampAxis(_trackedTile.x, nx, offset);
			if (cy != 0) ny = clampAxis(_trackedTile.y, ny, offset);
		}
	}

	// don't walk into the walls of the tile we're in
	const double lowX = _trackedTile.x * 16.0 + offset, lowY = _trackedTile.y * 16.0 + offset;
	if (nx < lowX + EdgeMargin && !stepAllowed(unit, -1, 0)) nx = lowX + EdgeMargin;
	if (nx > lowX + 16.0 - EdgeMargin && !stepAllowed(unit, 1, 0)) nx = lowX + 16.0 - EdgeMargin;
	if (ny < lowY + EdgeMargin && !stepAllowed(unit, 0, -1)) ny = lowY + EdgeMargin;
	if (ny > lowY + 16.0 - EdgeMargin && !stepAllowed(unit, 0, 1)) ny = lowY + 16.0 - EdgeMargin;
	// whatever stopped us (walls, edges, a step not taken) takes the momentum out on that axis
	if (dt > 0.0)
	{
		const double ax = (nx - _posX) / dt, ay = (ny - _posY) / dt;
		if (std::abs(ax) < std::abs(_velX) - 0.5) _velX = std::abs(ax) < 1.0 ? 0.0 : ax;
		if (std::abs(ay) < std::abs(_velY) - 0.5) _velY = std::abs(ay) < 1.0 ? 0.0 : ay;
	}
	const double moved = std::sqrt((nx - _posX) * (nx - _posX) + (ny - _posY) * (ny - _posY));
	footsteps(unit, moved);
	_bobPhase += moved;
	_posX = nx;
	_posY = ny;
}

/**
 * Footstep sounds while walking in the 3D view (the walking state only plays them for
 * animated walks on the isometric map): the tile's footstep pair, or the unit's own sound.
 */
void FirstPersonMode::footsteps(BattleUnit *unit, double distance)
{
	_footstepDistance += distance;
	if (_footstepDistance < 8.0)
	{
		return;
	}
	_footstepDistance = 0.0;
	Tile *tile = unit->getTile();
	if (!tile)
	{
		return;
	}
	int sound = Mod::NO_SOUND;
	if (unit->getMoveSound() != Mod::NO_SOUND)
	{
		if (_footstepPhase == 0) sound = unit->getMoveSound();
	}
	else
	{
		const int offset = tile->getFootstepSound(_save->getBelowTile(tile));
		if (offset > -1) sound = Mod::WALK_OFFSET + offset * 2 + _footstepPhase;
	}
	_footstepPhase ^= 1;
	if (sound != Mod::NO_SOUND)
	{
		if (Sound *s = _game->getMod()->getSoundByDepth(_save->getDepth(), sound))
		{
			s->play();
		}
	}
}

// ---------------------------------------------------------------------------------------
// weapons, fire modes, aiming
// ---------------------------------------------------------------------------------------

/**
 * The weapon in the chosen hand, or in the other hand if that one is empty.
 */
BattleItem *FirstPersonMode::activeWeapon(BattleUnit *unit) const
{
	auto it = _hands.find(unit);
	const int hand = it != _hands.end() ? it->second : 0; // each soldier starts with the right hand
	BattleItem *right = unit->getRightHandWeapon(), *left = unit->getLeftHandWeapon();
	BattleItem *chosen = hand ? left : right;
	return chosen ? chosen : (hand ? right : left);
}

/**
 * Dual wield (option): the gun in the other hand, drawn on its side and fired together with
 * the chosen one; null if either isn't a gun (a guided launcher doesn't count).
 */
BattleItem *FirstPersonMode::offHand(BattleUnit *unit, const BattleItem *weapon) const
{
	if (!Options::fpsDualWield || !unit || !weapon || weapon->getRules()->getBattleType() != BT_FIREARM || weapon->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}
	BattleItem *other = weapon == unit->getRightHandWeapon() ? unit->getLeftHandWeapon() : unit->getRightHandWeapon();
	if (!other || other == weapon || other->getRules()->getBattleType() != BT_FIREARM || other->getCurrentWaypoints() != 0)
	{
		return nullptr;
	}
	return other;
}

/**
 * Dual wield: the other hand's gun, if it can fire along with a fire mode of the chosen one,
 * and the mode it fires: the same one if it has it, else its first shot (its own time units).
 */
BattleItem *FirstPersonMode::dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &mode) const
{
	BattleItem *other = isShot(slot) ? offHand(unit, weapon) : nullptr;
	if (!other)
	{
		return nullptr;
	}
	const std::vector<BattleActionType> modes = modesFor(unit, other);
	BattleActionType pick = BA_NONE;
	if (std::find(modes.begin(), modes.end(), slot) != modes.end())
	{
		pick = slot;
	}
	else
	{
		for (BattleActionType m : modes)
		{
			if (isShot(m)) { pick = m; break; }
		}
	}
	if (pick == BA_NONE)
	{
		return nullptr;
	}
	mode = firedAs(other, pick);
	// its rounds fly beside this gun's, round for round: no more of them than this one fires
	const int shots = weapon->getActionConf(firedAs(weapon, slot))->shots;
	if (other->getActionConf(mode)->shots > shots)
	{
		mode = BA_NONE;
		for (BattleActionType m : { BA_SNAPSHOT, BA_AIMEDSHOT, BA_AUTOSHOT })
		{
			if (std::find(modes.begin(), modes.end(), m) != modes.end() && other->getActionConf(m)->shots <= shots) { mode = m; break; }
		}
		if (mode == BA_NONE)
		{
			return nullptr;
		}
	}
	return other->getAmmoForAction(mode) ? other : nullptr;
}

/**
 * Dual wield: swaps the other hand's gun (picture, shape, voxels) in for drawing, and back.
 */
void FirstPersonMode::swapOffHand()
{
	std::swap(_handSprite, _offSprite);
	std::swap(_vmRules, _offRules);
	std::swap(_vmShape, _offShape);
	std::swap(_vmFaces, _offFaces);
}

/**
 * What the left mouse button can do with an item, in the order the mouse wheel cycles
 * through them; mirrors the game's action menu.
 */
std::vector<BattleActionType> FirstPersonMode::modesFor(BattleUnit *unit, BattleItem *weapon) const
{
	std::vector<BattleActionType> modes;
	if (!weapon)
	{
		return modes;
	}
	const RuleItem *rules = weapon->getRules();
	const bool psiBlocked = rules->isPsiRequired() && unit->getBaseStats()->psiSkill <= 0;
	if (!psiBlocked)
	{
		if (rules->getBattleType() == BT_FIREARM)
		{
			const bool launcher = weapon->getCurrentWaypoints() != 0;
			const int slotLauncher = weapon->getActionConf(BA_LAUNCH)->ammoSlot;
			if ((!launcher || slotLauncher != weapon->getActionConf(BA_SNAPSHOT)->ammoSlot) && rules->getCostSnap().Time > 0) modes.push_back(BA_SNAPSHOT);
			if (!launcher && rules->getCostAimed().Time > 0) modes.push_back(BA_AIMEDSHOT);
			if ((!launcher || slotLauncher != weapon->getActionConf(BA_AUTOSHOT)->ammoSlot) && rules->getCostAuto().Time > 0) modes.push_back(BA_AUTOSHOT);
			if (launcher) modes.push_back(BA_LAUNCH);
		}
		if (rules->getCostMelee().Time > 0) modes.push_back(BA_HIT);
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
	if (!rules->isFixed() && rules->getCostThrow().Time > 0) modes.push_back(BA_THROW);
	if (!psiBlocked && rules->getFuseTimerType() != BFT_NONE && weapon->getFuseTimer() == -1 && rules->getCostPrime().Time > 0) modes.push_back(BA_PRIME);
	return modes;
}

/**
 * Keeps the fire mode valid for the weapon in hand. A fire mode picked on the classic HUD
 * (its weapon buttons) is taken over.
 */
void FirstPersonMode::updateFireMode(BattleUnit *unit)
{
	BattleAction *action = _state->_battleGame->getCurrentAction();
	// (not the auto a BURST fired just now in FPS style: the quick slot stays picked)
	const bool ownBurst = action->type != _mode && action->type == firedAs(action->weapon, _mode);
	if (action->targeting && action->actor == unit && action->weapon && action->type != BA_NONE && !ownBurst)
	{
		_hands[unit] = action->weapon == unit->getLeftHandWeapon() ? 1 : 0;
		_mode = action->type;
		_modeWeapon = action->weapon;
		return;
	}
	BattleItem *weapon = activeWeapon(unit);
	const std::vector<BattleActionType> modes = modesFor(unit, weapon);
	if (weapon != _modeWeapon || std::find(modes.begin(), modes.end(), _mode) == modes.end())
	{
		_mode = modes.empty() ? BA_NONE : modes.front();
		_modeWeapon = weapon;
	}
}

/**
 * Next/previous fire mode (mouse wheel, F).
 */
void FirstPersonMode::cycleMode(int step)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canCommit())
	{
		return;
	}
	_state->_battleGame->cancelCurrentAction();
	updateFireMode(unit);
	const std::vector<BattleActionType> modes = modesFor(unit, activeWeapon(unit));
	if (modes.empty())
	{
		return;
	}
	const int n = (int)modes.size();
	const int current = (int)(std::find(modes.begin(), modes.end(), _mode) - modes.begin());
	_mode = modes[((current + step) % n + n) % n];
}

/**
 * Picks a fire mode directly (snap, aimed, auto keys), if the weapon has it.
 */
void FirstPersonMode::selectMode(BattleActionType mode)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canCommit())
	{
		return;
	}
	const std::vector<BattleActionType> modes = modesFor(unit, activeWeapon(unit));
	if (std::find(modes.begin(), modes.end(), mode) != modes.end())
	{
		_state->_battleGame->cancelCurrentAction();
		_mode = mode;
		_modeWeapon = activeWeapon(unit);
	}
}

/**
 * Switches the weapon hand (G).
 */
void FirstPersonMode::switchHand()
{
	if (!canCommit())
	{
		return;
	}
	_state->_battleGame->cancelCurrentAction();
	if (BattleUnit *unit = _save->getSelectedUnit())
	{
		_hands[unit] ^= 1;
	}
	_modeWeapon = nullptr;
}

/**
 * The first voxel along the ray through a pixel of the view (units included), like a shot
 * would see it. Off into the sky, a point far along the ray.
 * @return True if something was hit.
 */
bool FirstPersonMode::aimAt(int px, int py, BattleUnit *unit, Position &voxel)
{
	double eye[3], dir[3];
	_view->getEye(eye);
	_view->getRayDirection(px, py, dir);
	const Position origin((int)eye[0], (int)eye[1], (int)eye[2]);
	const Position far(origin.x + (int)(dir[0] * 1500), origin.y + (int)(dir[1] * 1500), origin.z + (int)(dir[2] * 1500));
	std::vector<Position> trajectory;
	const int hit = _save->getTileEngine()->calculateLineVoxel(origin, far, false, &trajectory, unit, nullptr, !_save->getDebugMode());
	if (hit != V_EMPTY && hit != V_OUTOFBOUNDS && !trajectory.empty())
	{
		voxel = trajectory.front();
		return true;
	}
	voxel = Position(origin.x + (int)(dir[0] * 320), origin.y + (int)(dir[1] * 320), origin.z + (int)(dir[2] * 320));
	return false;
}

/**
 * What's under the crosshair this frame.
 */
void FirstPersonMode::aim(BattleUnit *unit)
{
	_aimValid = aimAt(_view->getWidth() / 2, _view->getHeight() / 2, unit, _aimVoxel);
}

/**
 * Uses the fire mode on whatever is under a pixel of the view, through the game's own
 * actions (TU cost, accuracy, reaction fire): shots, throws and the launcher target it,
 * melee hits what's in front, psi targets the unit there; medikits, scanners and priming
 * open the usual action menu.
 */
void FirstPersonMode::fire(int px, int py)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canCommit() || !_state->playableUnitSelected())
	{
		return;
	}
	updateFireMode(unit);
	BattleItem *weapon = activeWeapon(unit);
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();
	BattleActionType mode = _mode;
	// an action picked from the item menu that wants a target (mind probe, psi from the menu...):
	// this click is its target
	const bool menuTargeting = action->targeting && action->actor == unit && action->weapon;
	if (menuTargeting)
	{
		weapon = action->weapon;
		mode = action->type;
	}
	if (!weapon || mode == BA_NONE)
	{
		return;
	}
	if (mode == BA_LAUNCH)
	{
		startMapAim(unit, weapon, BA_LAUNCH, "STR_FPS_LAUNCH_HINT"); // waypoints are placed on the isometric map
		return;
	}
	// psi and the mind probe reach creatures out of sight: with none under the crosshair,
	// pick one on the isometric map
	const bool mindProbe = mode == BA_USE && menuTargeting && weapon->getRules()->getBattleType() == BT_MINDPROBE;
	if (mode == BA_PANIC || mode == BA_MINDCONTROL || mindProbe)
	{
		Position voxel;
		aimAt(px, py, unit, voxel);
		if (!creatureAt(voxel, unit))
		{
			startMapAim(unit, weapon, mode, "STR_FPS_PSI_HINT");
			return;
		}
	}
	if ((mode == BA_USE || mode == BA_PRIME) && !menuTargeting)
	{
		_state->handleItemClick(weapon, false); // the item's own menu: medi-kit, scanner, priming, ...
		return;
	}
	if (mode == BA_HIT)
	{
		Position target;
		if (!_save->getTileEngine()->validMeleeRange(unit->getPosition(), unit->getDirection(), unit, nullptr, &target))
		{
			_state->warning("STR_THERE_IS_NO_ONE_THERE");
			return;
		}
		action->actor = unit;
		action->weapon = weapon;
		action->type = BA_HIT;
		action->targeting = false;
		action->target = target;
		action->result.clear();
		action->updateTU();
		battleGame->handleNonTargetAction();
		if (action->result.empty())
		{
			_vmSwingStart = SDL_GetTicks(); // (refused for lack of time units, it has a result)
			_swingJolted = false;
			_sounds->play(FirstPersonSounds::SWOOSH, 0, 0.8f);
		}
		return;
	}

	const BattleActionType slot = mode;
	mode = firedAs(weapon, mode); // FPS style: the quick slot fires a burst
	if (isShot(mode) && !weapon->getAmmoForAction(mode))
	{
		_sounds->play(FirstPersonSounds::DRY_FIRE); // click: nothing in it
	}

	Position voxel;
	const bool hit = aimAt(px, py, unit, voxel);
	Position target = voxel.toTile();
	target.x = std::clamp<int>(target.x, 0, _save->getMapSizeX() - 1);
	target.y = std::clamp<int>(target.y, 0, _save->getMapSizeY() - 1);
	target.z = std::clamp<int>(target.z, 0, _save->getMapSizeZ() - 1);

	action->actor = unit;
	action->weapon = weapon;
	action->type = mode;
	action->targeting = true;
	action->waypoints.clear();
	action->updateTU();
	action->freeAim = freeAimOn(mode); // FPS style: each round asked for at its moment (freeAimTarget)
	if ((Options::fpsAimExact || action->freeAim) && hit && isShot(mode))
	{
		action->targetVoxel = voxel; // aim at that exact point instead of a point the game picks on the tile
	}
	// dual wield (option): the gun in the other hand fires at the same moment, round for round,
	// both half as accurate; with time for only one, just the chosen hand fires, at full accuracy
	BattleActionType offMode = BA_NONE;
	BattleItem *other = menuTargeting ? nullptr : dualPartner(unit, weapon, slot, offMode);
	if (other && !Options::fpsDoomMode && action->Time + BattleActionCost(offMode, unit, other).Time > unit->getTimeUnits())
	{
		other = nullptr;
	}
	action->dualWield = other != nullptr;
	action->dualWeapon = other;
	action->dualType = offMode;
	battleGame->primaryAction(target);
	if (Options::battleConfirmFireMode && action->targeting && !action->waypoints.empty())
	{
		battleGame->primaryAction(target); // the first click only marked the target
	}
	action->targetVoxel = Position(-1, -1, -1);
	action->freeAim = false;
	action->dualWield = false;
	action->dualWeapon = nullptr;
	action->dualType = BA_NONE;
}

/**
 * What a fire mode fires: as it says, except in FPS style (setting "Faithful X-COM targeting"
 * off) the quick slot of a gun that has auto fires one burst (the game's auto: its rounds, its
 * time units), while auto keeps firing while the trigger is held.
 */
BattleActionType FirstPersonMode::firedAs(const BattleItem *weapon, BattleActionType mode) const
{
	if (mode != BA_SNAPSHOT || !freeAimOn(mode) || !weapon)
	{
		return mode;
	}
	const RuleItem *rules = weapon->getRules();
	return rules->getCostAuto().Time > 0 && weapon->getCurrentWaypoints() == 0 ? BA_AUTOSHOT : mode;
}

/**
 * Is this fire mode FPS style just now (setting "Faithful X-COM targeting" off, a gun's shots)?
 */
bool FirstPersonMode::freeAimOn(BattleActionType mode) const
{
	return !Options::fpsFaithfulTargeting && isShot(mode);
}

/**
 * How wide an FPS-style shot spreads: the angle (degrees) 95% of rounds land within. Aimed shots
 * are tight, snap shots from the hip wider, auto in between before the recoil; the soldier's
 * firing skill and kneeling tighten it, as they do the game's accuracy.
 */
double FirstPersonMode::spreadFor(const BattleUnit *unit, BattleActionType mode, bool dual) const
{
	double r95 = mode == BA_AIMEDSHOT ? 0.6 : mode == BA_AUTOSHOT ? 1.6 : 2.2;
	r95 *= std::clamp(1.5 - unit->getBaseStats()->firing / 100.0, 0.6, 1.3);
	if (unit->isKneeled()) r95 *= 0.7;
	if (dual) r95 *= 2.0; // a gun in each hand: half as accurate
	return r95;
}

void FirstPersonMode::freeAim(BattlescapeState *state, const BattleAction &action, Position &target)
{
	if (state && state->_firstPerson && state->_firstPerson->isActive())
	{
		state->_firstPerson->freeAimTarget(action, target);
	}
}

/**
 * FPS-style shooting in the 3D view: our own bullet hits play quicker, so the next round of a
 * burst doesn't wait for the last one's puff (the game's pace is about a round a second).
 */
bool FirstPersonMode::fastShots(BattlescapeState *state, const BattleUnit *attacker)
{
	return state && state->_firstPerson && state->_firstPerson->isActive() && !Options::fpsFaithfulTargeting
		&& attacker && attacker->getFaction() == FACTION_PLAYER;
}

/**
 * The next round of an FPS-style shot: along the crosshair as it points now (the recoil of the
 * rounds before has lifted it), spread at random by the mode and the soldier, then bloomed a
 * little for the next one. The game carries the line on through that point and does the rest
 * (what it hits, the damage).
 */
void FirstPersonMode::freeAimTarget(const BattleAction &action, Position &target)
{
	BattleUnit *unit = action.actor;
	if (!unit)
	{
		return;
	}
	const double r95 = spreadFor(unit, action.type, action.dualWield) + _bloom;
	const double sigma = r95 / 2.45 * Pi / 180.0; // 95% of a 2D normal spread lies within 2.45 sigma
	auto normal = [] {
		const double u = std::max(1e-9, RNG::generate(0.0, 1.0)), v = RNG::generate(0.0, 1.0);
		return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * Pi * v);
	};
	const double ax = normal() * sigma, ay = normal() * sigma;
	const int w = _view->getWidth(), h = _view->getHeight();
	const double focal = (w / 2.0) / std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0);
	const int px = std::clamp((int)std::lround(w / 2.0 + std::tan(ax) * focal), 0, w - 1);
	const int py = std::clamp((int)std::lround(h / 2.0 - std::tan(ay) * focal), 0, h - 1);
	Position voxel;
	if (aimAt(px, py, unit, voxel))
	{
		target = voxel;
	}
	else
	{
		double eye[3], dir[3];
		_view->getEye(eye);
		_view->getRayDirection(px, py, dir);
		target = Position((int)(eye[0] + dir[0] * 1500), (int)(eye[1] + dir[1] * 1500), (int)(eye[2] + dir[2] * 1500));
	}
	_bloom = std::min(_bloom + (action.type == BA_AUTOSHOT ? 0.45 : 0.3), 3.0);
}

/**
 * FPS-style recoil: each of our rounds lifts the view by the weapon's power (lasers barely),
 * with a little sideways jitter; after a pause the view eases back down, and the bloom fades.
 */
void FirstPersonMode::updateRecoil(BattleUnit *unit, double dt)
{
	const Uint32 now = SDL_GetTicks();
	const Uint32 shot = _view->getOwnShotTick() ^ (_view->getPartnerShotTick() << 1); // (dual wield: either gun's round)
	if (shot != _recoilShotSeen)
	{
		_recoilShotSeen = shot;
		BattleItem *weapon = unit ? activeWeapon(unit) : nullptr;
		if (!Options::fpsFaithfulTargeting && weapon && weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			const BattleItem *ammo = weapon->getAmmoForSlot(0);
			const RuleItem *rules = ammo ? ammo->getRules() : weapon->getRules();
			const RuleDamageType *damage = rules->getDamageType();
			const double weight = damage && damage->ResistType == DT_LASER ? 0.4 : damage && damage->ResistType == DT_PLASMA ? 0.7 : 1.0;
			const BattleActionType fired = firedAs(weapon, _mode);
			const double mode = fired == BA_AUTOSHOT ? 1.0 : fired == BA_SNAPSHOT ? 0.8 : 0.6;
			const double climb = std::clamp(rules->getPower() / 40.0, 0.4, 2.0) * weight * mode;
			_view->rotate(RNG::generate(-0.35, 0.35) * climb, climb);
			_recoilDebt += climb;
			_lastRecoil = now;
		}
	}
	if (now - _lastRecoil > 180)
	{
		if (_recoilDebt > 0.0)
		{
			const double step = std::min(_recoilDebt, 9.0 * dt);
			_view->rotate(0.0, -step);
			_recoilDebt -= step;
		}
		_bloom = std::max(0.0, _bloom - 5.0 * dt);
	}
}

// ---------------------------------------------------------------------------------------
// crosshair, panel, banner
// ---------------------------------------------------------------------------------------

/**
 * Draws an X-COM style crosshair for a fire mode, with a dark outline so it reads on any
 * background: snap = open cross, aimed = scope ring, auto = brackets with one dot per round
 * of the burst, throw = arc onto a landing ring, launcher = diamond, melee = claw marks,
 * psi = rings, items used from the menu = box.
 */
void FirstPersonMode::drawCrosshair(BattleActionType mode, int shots, Uint8 color, Uint8 shadow, int hitMark, Uint8 hitColor)
{
	const int c = CrosshairSize / 2;
	std::vector<std::pair<int, int>> points;
	auto dot = [&](int x, int y) { points.push_back({ c + x, c + y }); };
	auto line = [&](int x0, int y0, int x1, int y1) {
		const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		while (true)
		{
			dot(x0, y0);
			if (x0 == x1 && y0 == y1) break;
			const int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	};
	auto circle = [&](int r, int every) {
		int n = 0;
		for (int a = 0; a < 360; a += 4)
		{
			if (n++ % every) continue;
			dot((int)std::lround(r * std::cos(a * Pi / 180.0)), (int)std::lround(r * std::sin(a * Pi / 180.0)));
		}
	};

	switch (mode)
	{
	case BA_SNAPSHOT:
		line(-9, 0, -4, 0); line(4, 0, 9, 0); line(0, -9, 0, -4); line(0, 4, 0, 9);
		dot(0, 0);
		break;
	case BA_AIMEDSHOT:
		circle(9, 1);
		line(-12, 0, -5, 0); line(5, 0, 12, 0); line(0, -12, 0, -5); line(0, 5, 0, 12);
		dot(0, 0);
		break;
	case BA_AUTOSHOT:
	{
		line(-10, -5, -10, 5); line(-10, -5, -8, -5); line(-10, 5, -8, 5);
		line(10, -5, 10, 5); line(10, -5, 8, -5); line(10, 5, 8, 5);
		line(-3, 0, 3, 0); line(0, -3, 0, 3);
		const int n = std::clamp(shots, 1, 6);
		for (int i = 0; i < n; ++i)
		{
			const int x = i * 4 - (n - 1) * 2;
			dot(x, 9); dot(x + 1, 9); dot(x, 10); dot(x + 1, 10);
		}
		break;
	}
	case BA_THROW:
		for (int x = -12; x <= 12; x += 3)
		{
			dot(x, -8 + (x * x) / 18);
		}
		circle(4, 1);
		dot(0, 0);
		break;
	case BA_LAUNCH:
		line(0, -10, 10, 0); line(10, 0, 0, 10); line(0, 10, -10, 0); line(-10, 0, 0, -10);
		line(-2, 0, 2, 0); line(0, -2, 0, 2);
		break;
	case BA_HIT:
		line(-9, -6, -5, 6); line(-2, -8, 2, 8); line(5, -6, 9, 6);
		break;
	case BA_PANIC:
	case BA_MINDCONTROL:
		circle(4, 2); circle(9, 2); circle(14, 3);
		dot(0, 0);
		break;
	case BA_USE:
	case BA_PRIME:
		line(-5, -5, 5, -5); line(5, -5, 5, 5); line(5, 5, -5, 5); line(-5, 5, -5, -5);
		dot(0, 0);
		break;
	default:
		dot(0, 0); dot(-1, 0); dot(1, 0); dot(0, -1); dot(0, 1);
		break;
	}

	// hitmarker: four ticks on the diagonals, white for a hit, red for a kill
	const size_t modePoints = points.size();
	if (hitMark)
	{
		const int r0 = hitMark == 2 ? 6 : 7, r1 = hitMark == 2 ? 12 : 10;
		line(-r0, -r0, -r1, -r1); line(r0, -r0, r1, -r1); line(-r0, r0, -r1, r1); line(r0, r0, r1, r1);
	}

	_crosshair->clear();
	for (const auto &p : points)
	{
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
				_crosshair->setPixel(p.first + dx, p.second + dy, shadow);
	}
	for (size_t i = 0; i < points.size(); ++i)
	{
		_crosshair->setPixel(points[i].first, points[i].second, i < modePoints ? color : hitColor);
	}
}

/**
 * Updates the crosshair and its labels: mode name (with the burst size), hit chance at what's
 * under the crosshair (like the isometric cursor, with range dropoff), and the TU cost;
 * yellow when you can, red when you can't, grey when it's not your move.
 */
void FirstPersonMode::updateCrosshair(BattleUnit *unit, bool playerTurn)
{
	const bool show = Options::fpsCrosshair && isActive();
	_crosshair->setVisible(show);
	_txtMode->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	_txtShot->setVisible(show && Options::fpsCrosshairInfo && playerTurn);
	if (!show)
	{
		return;
	}
	const SDL_Color *pal = _view->getPalette();
	const Uint8 yellow = nearestColor(pal, 255, 216, 64), red = nearestColor(pal, 232, 56, 40);
	const Uint8 grey = nearestColor(pal, 150, 150, 150), shadow = nearestColor(pal, 8, 8, 8), psi = nearestColor(pal, 200, 120, 255);
	const Uint8 yellowText = textColor(pal, 255, 216, 64), redText = textColor(pal, 240, 70, 50);
	const Uint8 greyText = textColor(pal, 170, 170, 170), psiText = textColor(pal, 200, 120, 255), greenText = textColor(pal, 130, 230, 100);

	BattleItem *weapon = (playerTurn && unit) ? activeWeapon(unit) : nullptr;
	if (playerTurn && unit)
	{
		const BattleAction *action = _state->_battleGame->getCurrentAction();
		if (action->targeting && action->actor == unit && action->weapon)
		{
			weapon = action->weapon;
		}
	}
	const BattleActionType slot = (playerTurn && weapon) ? _mode : BA_NONE;
	const BattleActionType mode = firedAs(weapon, slot); // (FPS style: the quick slot is a burst)
	int shots = 1;
	Uint8 color = grey;
	std::string modeText, shotText;
	bool dual = false;
	if (weapon && mode != BA_NONE && unit)
	{
		const RuleItem *rules = weapon->getRules();
		if (mode == BA_AUTOSHOT) shots = rules->getConfigAuto()->shots;
		const BattleActionCost cost(mode, unit, weapon);
		bool can = Options::fpsDoomMode || cost.Time <= unit->getTimeUnits();
		if (isShot(mode) || mode == BA_LAUNCH)
		{
			can = can && (!weapon->needsAmmoForAction(mode) || weapon->getAmmoForAction(mode));
		}
		// dual wield: the other hand's gun joins in if there's time for both
		BattleActionType offMode = BA_NONE;
		int offTime = 0;
		if (BattleItem *other = can ? dualPartner(unit, weapon, slot, offMode) : nullptr)
		{
			offTime = BattleActionCost(offMode, unit, other).Time;
			if (!Options::fpsDoomMode && cost.Time + offTime > unit->getTimeUnits()) offTime = -1;
		}
		dual = offTime > 0 || (offTime == 0 && offMode != BA_NONE);

		std::ostringstream ss;
		int accuracy = -1;
		Uint8 accuracyColor = greenText;
		if ((isShot(mode) || mode == BA_THROW) && _aimValid && !freeAimOn(mode)) // (FPS style: no odds, the spread ring shows it)
		{
			const BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(mode, unit, weapon);
			accuracy = BattleUnit::getFiringAccuracy(attack, _game->getMod());
			if (dual) accuracy /= 2; // (before the range falloff, as the shot does)
			const int distanceSq = unit->distance3dToPositionSq(_aimVoxel.toTile());
			const int distance = (int)std::ceil(std::sqrt((double)distanceSq));
			int upper, lower;
			const int dropoff = rules->calculateLimits(upper, lower, _save->getDepth(), mode);
			if (distance > upper) { accuracy -= (distance - upper) * dropoff; accuracyColor = yellowText; }
			else if (distance < lower) { accuracy -= (lower - distance) * dropoff; accuracyColor = yellowText; }
			const bool outOfRange = mode == BA_THROW ? rules->isOutOfThrowRange(distanceSq, _save->getDepth()) : rules->isOutOfRange(distanceSq);
			if (accuracy <= 0 || outOfRange)
			{
				accuracy = 0;
				accuracyColor = redText;
				can = false;
			}
			ss << accuracy << "%  ";
		}
		ss << cost.Time + (dual ? offTime : 0) << std::string(_state->tr("STR_FPS_TU"));
		shotText = ss.str();

		static const struct { BattleActionType mode; const char *name; } names[] = {
			{ BA_SNAPSHOT, "STR_FPS_MODE_SNAP" }, { BA_AIMEDSHOT, "STR_FPS_MODE_AIMED" }, { BA_AUTOSHOT, "STR_FPS_MODE_AUTO" },
			{ BA_THROW, "STR_FPS_MODE_THROW" }, { BA_LAUNCH, "STR_FPS_MODE_LAUNCH" }, { BA_HIT, "STR_FPS_MODE_MELEE" },
			{ BA_PANIC, "STR_FPS_MODE_PANIC" }, { BA_MINDCONTROL, "STR_FPS_MODE_MIND_CONTROL" }, { BA_USE, "STR_FPS_MODE_USE" },
			{ BA_PRIME, "STR_FPS_MODE_PRIME" } };
		for (const auto &n : names)
		{
			if (n.mode == mode) modeText = _state->tr(n.name);
		}
		if (mode == BA_AUTOSHOT && slot != mode)
		{
			modeText = std::string(_state->tr("STR_FPS_MODE_BURST")) + " x" + std::to_string(shots); // FPS style quick slot
		}
		else if (mode == BA_AUTOSHOT)
		{
			modeText += freeAimOn(mode) ? std::string(" ") + std::string(_state->tr("STR_FPS_HOLD")) : " x" + std::to_string(shots);
		}
		if (dual)
		{
			modeText = std::string(_state->tr("STR_FPS_DUAL")) + " " + modeText;
		}
		color = !can ? red : (mode == BA_PANIC || mode == BA_MINDCONTROL) ? psi : yellow;
		Uint8 labelColor = !can ? redText : (mode == BA_PANIC || mode == BA_MINDCONTROL) ? psiText : yellowText;
		if (!canCommit())
		{
			color = grey;
			labelColor = greyText;
		}
		_txtMode->setColor(labelColor);
		_txtShot->setColor(accuracy >= 0 && can ? accuracyColor : labelColor);
	}
	_txtMode->setText(modeText);
	_txtShot->setText(shotText);

	// a hit (or kill) on an enemy in sight marks the crosshair for a moment
	const Uint32 hitTick = _view->getHitMarkTick();
	if (hitTick != _hitMarkHeard)
	{
		_hitMarkHeard = hitTick;
		_sounds->play(_view->getHitMarkKill() ? FirstPersonSounds::KILL : FirstPersonSounds::HIT, 0, 0.7f);
	}
	const int hitMark = hitTick && SDL_GetTicks() - hitTick < 280 ? (_view->getHitMarkKill() ? 2 : 1) : 0;
	const Uint8 hitColor = hitMark == 2 ? red : nearestColor(pal, 255, 255, 255);
	// FPS style: a dotted ring shows the spread (it grows with the bloom of the last rounds)
	int ring = 0;
	if (unit && freeAimOn(mode))
	{
		const double focal = (_view->getWidth() / 2.0) / std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0);
		ring = std::clamp((int)std::lround(std::tan((spreadFor(unit, mode, dual) + _bloom) * Pi / 180.0) * focal), 2, CrosshairSize / 2 - 1);
	}
	const Uint32 key = (Uint32)mode | ((Uint32)shots << 8) | ((Uint32)color << 16) | ((Uint32)hitMark << 24) | ((Uint32)ring << 26);
	if (key != _crosshairKey)
	{
		_crosshairKey = key;
		drawCrosshair(mode, shots, color, shadow, hitMark, hitColor);
		if (ring)
		{
			_crosshair->lock();
			const int c = CrosshairSize / 2;
			for (int a = 0; a < 360; a += 12)
			{
				const int x = c + (int)std::lround(ring * std::cos(a * Pi / 180.0)), y = c + (int)std::lround(ring * std::sin(a * Pi / 180.0));
				_crosshair->setPixel(x + 1, y + 1, shadow);
				_crosshair->setPixel(x, y, color);
			}
			_crosshair->unlock();
		}
	}
}

/**
 * Doom mode (debug): the selected soldier never runs out of time units, energy or ammo.
 */
void FirstPersonMode::applyDoom(BattleUnit *unit)
{
	unit->resetTimeUnitsAndEnergy();
	for (BattleItem *item : { unit->getRightHandWeapon(), unit->getLeftHandWeapon() })
	{
		if (!item)
		{
			continue;
		}
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
 * Fills the info panel: name, weapon in hand with its ammo, kneeling, the four stats.
 */
void FirstPersonMode::updatePanel(BattleUnit *unit)
{
	const bool show = isActive() && !_classicHud && unit;
	for (auto *part : _panelParts)
	{
		part->setVisible(show);
	}
	_txtDoom->setVisible(show && Options::fpsDoomMode);
	if (!show)
	{
		return;
	}
	_txtName->setText(unit->getName(_game->getLanguage(), false));
	_txtWeapon->setText(weaponLabel(unit));
	_txtStatus->setText(statusLabel(unit));
	_numTimeUnits->setValue(unit->getTimeUnits());
	_barTimeUnits->setMax(unit->getBaseStats()->tu);
	_barTimeUnits->setValue(unit->getTimeUnits());
	_numEnergy->setValue(unit->getEnergy());
	_barEnergy->setMax(unit->getBaseStats()->stamina);
	_barEnergy->setValue(unit->getEnergy());
	_numHealth->setValue(unit->getHealth());
	_barHealth->setMax(unit->getBaseStats()->health);
	_barHealth->setValue(unit->getHealth());
	_barHealth->setValue2(unit->getStunlevel());
	_numMorale->setValue(unit->getMorale());
	_barMorale->setMax(100);
	_barMorale->setValue(unit->getMorale());
}

/**
 * "R: Rifle (20)": the weapon in the active hand and its ammo.
 */
std::string FirstPersonMode::weaponLabel(BattleUnit *unit) const
{
	std::string text;
	if (BattleItem *weapon = activeWeapon(unit))
	{
		auto rounds = [](const BattleItem *item) {
			const BattleItem *ammo = item->getAmmoForSlot(0);
			return ammo && item->getRules()->getBattleType() == BT_FIREARM ? std::to_string(ammo->getAmmoQuantity()) : std::string("-");
		};
		const std::string name = _state->tr(weapon->getRules()->getName());
		if (const BattleItem *other = offHand(unit, weapon))
		{
			// dual wield: "R+L: Pistol (12|9)", or "Rifle (20) + Pistol (9)" (the chosen hand's first)
			if (other->getRules() == weapon->getRules())
			{
				return "R+L: " + name + " (" + rounds(weapon) + "|" + rounds(other) + ")";
			}
			return name + " (" + rounds(weapon) + ") + " + std::string(_state->tr(other->getRules()->getName())) + " (" + rounds(other) + ")";
		}
		text = std::string(weapon == unit->getLeftHandWeapon() ? "L: " : "R: ") + name;
		const BattleItem *ammo = weapon->getAmmoForSlot(0);
		if (ammo && weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			text += " (" + std::to_string(ammo->getAmmoQuantity()) + ")";
		}
	}
	return text;
}

/**
 * Bleeding wounds first (they need a medikit), else kneeling.
 */
std::string FirstPersonMode::statusLabel(BattleUnit *unit) const
{
	if (unit->getFatalWounds() > 0)
	{
		return _state->tr("STR_FPS_WOUNDS").arg(unit->getFatalWounds());
	}
	return unit->isKneeled() ? std::string(_state->tr("STR_FPS_KNEELING")) : std::string();
}

/**
 * The armor line under the stats: the armor's name, and buttons for what it can do: FLY (SWIM
 * under water) for flying armor, LIGHT for its personal light, SPECIAL for a built-in weapon.
 */
void FirstPersonMode::updateArmor(BattleUnit *unit)
{
	const bool show = isActive() && unit;
	BattleType specialType;
	const bool fly = show && canFly(unit), light = show && unit->getArmor()->getPersonalLightFriend() > 0;
	const bool special = show && unit->getSpecialIconWeapon(specialType) != nullptr;
	const bool night = show && hasNightVision(unit);
	if (fly != _btnFly->getVisible() || light != _btnLight->getVisible() || night != _btnNight->getVisible() || special != _btnSpecial->getVisible())
	{
		_btnFly->setVisible(fly);
		_btnLight->setVisible(light);
		_btnNight->setVisible(night);
		_btnSpecial->setVisible(special);
		layoutTopBar(); // the squad buttons make room
	}
	if (!show)
	{
		return;
	}
	const std::string &type = unit->getArmor()->getType();
	_txtArmor->setText(type == "STR_NONE_UC" ? _state->tr("STR_FPS_NO_ARMOR") : _state->tr(type));
	TextButton *flyGroup = flightOn(unit) ? _btnFly : nullptr;
	TextButton *lightGroup = _save->getTogglePersonalLight() ? _btnLight : nullptr;
	TextButton *nightGroup = _state->_map->isNightVisionOn() ? _btnNight : nullptr;
	if (flyGroup != _flyGroup || lightGroup != _lightGroup || nightGroup != _nightGroup)
	{
		_flyGroup = flyGroup;
		_lightGroup = lightGroup;
		_nightGroup = nightGroup;
		_btnFly->invalidate();
		_btnLight->invalidate();
		_btnNight->invalidate();
	}
}

/**
 * The lift controls: shown while the soldier stands on a grav lift (or a ladder) that can take
 * it up or down from here, one arrow for each way it goes.
 */
void FirstPersonMode::updateLift(BattleUnit *unit, Uint32 now)
{
	bool up = false, down = false;
	const bool riding = _rideStart && now - _rideStart < _rideTime;
	if (unit && isActive() && !_classicHud && !_stepPending && !riding && canCommit() && unit->getStatus() == STATUS_STANDING)
	{
		const Tile *tile = unit->getTile();
		const MapData *floor = tile ? tile->getMapData(O_FLOOR) : nullptr;
		if (tile && ((floor && floor->isGravLift()) || tile->hasLadder()))
		{
			Pathfinding *pathfinding = _save->getPathfinding();
			up = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_UP, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST;
			down = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_DOWN, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST;
		}
	}
	_liftUp->setVisible(up);
	_liftDown->setVisible(down);
	_txtLift->setVisible(up || down);
	_txtLiftKeys->setVisible(up || down);
	if (!up && !down)
	{
		_liftHover = 0;
		return;
	}
	const int ways = (up ? 1 : 0) | (down ? 2 : 0);
	if (ways != _liftWays)
	{
		// the keys that do the same, in the game's spelling of them
		auto name = [](SDLKey key) { std::string text = SDL_GetKeyName(key); for (char &c : text) c = (char)toupper((unsigned char)c); return text; };
		std::string keys = up ? name(Options::keyFpsUp) : std::string();
		if (down) keys += (keys.empty() ? "" : " / ") + name(Options::keyFpsDown);
		_txtLiftKeys->setText(keys);
		_liftWays = ways;
	}
	if (up) drawLiftArrow(_liftUp, true, _liftHover == 1, now);
	if (down) drawLiftArrow(_liftDown, false, _liftHover == -1, now);
}

/**
 * A wide purple chevron, pointing up or down, gently pulsing (bright when the cursor is on it).
 */
void FirstPersonMode::drawLiftArrow(Surface *surface, bool up, bool hover, Uint32 now)
{
	const SDL_Color *pal = _state->getPalette();
	const double pulse = 0.5 + 0.5 * std::sin(now * 0.006);
	const Uint8 edge = nearestColor(pal, 50, 15, 90);
	const Uint8 fill = hover ? nearestColor(pal, 235, 190, 255) : nearestColor(pal, (int)(150 + 50 * pulse), (int)(60 + 40 * pulse), 255);
	const Uint8 rim = hover ? nearestColor(pal, 255, 235, 255) : nearestColor(pal, 210, 150, 255);
	const int w = surface->getWidth(), h = surface->getHeight();
	const double half = (w - 1) / 2.0, slope = (h - 4) / half, thickness = 2.2;
	auto inside = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= w || y >= h) return false;
		const double arm = 1.5 + std::abs(x - half) * slope; // the apex row at the top, for an up arrow
		return std::abs((up ? y : h - 1 - y) - arm) <= thickness;
	};
	surface->clear();
	surface->lock();
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			if (inside(x, y))
			{
				const bool top = !inside(x, up ? y - 1 : y + 1); // the leading edge catches the light
				surface->setPixel(x, y, top ? rim : fill);
			}
			else if (inside(x - 1, y) || inside(x + 1, y) || inside(x, y - 1) || inside(x, y + 1))
			{
				surface->setPixel(x, y, edge);
			}
		}
	}
	surface->unlock();
}

/**
 * Which lift arrow is at this canvas point: 1 up, -1 down, 0 none.
 */
int FirstPersonMode::liftAt(double x, double y) const
{
	auto on = [&](const Surface *s) {
		return s->getVisible() && x >= s->getX() - 3 && x < s->getX() + s->getWidth() + 3 && y >= s->getY() - 5 && y < s->getY() + s->getHeight() + 5;
	};
	return on(_liftUp) ? 1 : on(_liftDown) ? -1 : 0;
}

/**
 * Where a melee swing is: winding up (0..1, the first quarter), then the strike (0..1..0).
 * @return False when there's no swing on.
 */
bool FirstPersonMode::swingPhase(double &windup, double &strike) const
{
	windup = strike = 0.0;
	if (!_vmSwingStart)
	{
		return false;
	}
	const double t = (SDL_GetTicks() - _vmSwingStart) / (double)SwingTime;
	if (t >= 1.0)
	{
		return false;
	}
	if (t < 0.25) windup = std::sin(Pi / 2.0 * t / 0.25);
	else if (t < 0.5) { windup = std::cos(Pi / 2.0 * (t - 0.25) / 0.25); strike = std::sin(Pi / 2.0 * (t - 0.25) / 0.25); }
	else strike = std::cos(Pi / 2.0 * (t - 0.5) / 0.5);
	return true;
}

/**
 * An enemy shot whizzed past close by: hear it go past on its side, and flinch.
 */
void FirstPersonMode::checkNearMiss()
{
	const Uint32 tick = _view->getNearMissTick();
	if (tick == _nearMissSeen)
	{
		return;
	}
	_nearMissSeen = tick;
	double dir[2];
	_view->getNearMissDirection(dir);
	const double angle = std::fmod((std::atan2(dir[1], dir[0]) - _view->getYawDegrees() * Pi / 180.0) * 180.0 / Pi + 720.0, 360.0);
	_sounds->play(FirstPersonSounds::WHIZZ, (int)angle, 0.85f);
	_view->addShake(0.8f);
}

/**
 * A fall of some levels (a step off a ledge, setting "Falling"): the landing jolts, and hurts by
 * the height: a few points from one level, a lot from two, worse from more; armor takes some of
 * it, and from two levels a leg may be wounded.
 */
void FirstPersonMode::fallDamage(BattleUnit *unit, int levels)
{
	_view->addShake(1.5f + levels);
	footsteps(unit, 8.0);
	if (unit->isOut())
	{
		return;
	}
	RuleDamageType fall = *_game->getMod()->getDamageType(DT_MELEE);
	fall.RandomType = DRT_FLAT;     // the power below, as it is
	fall.IgnoreDirection = true;
	fall.ArmorEffectiveness = 0.5f;
	fall.ToArmor = 0.0f;
	fall.ToWound = levels >= 2 ? 0.5f : 0.0f;
	fall.ToStun = 0.5f;
	const int power = levels == 1 ? RNG::generate(2, 6) : levels * 10 - 8 + RNG::generate(0, 8);
	unit->damage(Position(0, 0, 0), power, &fall, _save, BattleActionAttack{}, SIDE_UNDER, RNG::generate(0, 1) ? BODYPART_LEFTLEG : BODYPART_RIGHTLEG);
	_state->_battleGame->checkForCasualties(&fall, BattleActionAttack{}, false, false);
}

/**
 * Can this unit fly (or swim) at all?
 */
bool FirstPersonMode::canFly(const BattleUnit *unit) const
{
	return unit && unit->getMovementType() == MT_FLY;
}

/**
 * Is flight switched on for this soldier? Until switched, a soldier already in the air flies.
 */
bool FirstPersonMode::flightOn(const BattleUnit *unit) const
{
	if (!canFly(unit))
	{
		return false;
	}
	auto it = _flight.find(unit);
	return it != _flight.end() ? it->second : unit->isFloating();
}

/**
 * Switches flight on or off; switched off in the air, the soldier comes down to land.
 */
void FirstPersonMode::toggleFlight(BattleUnit *unit)
{
	if (!canFly(unit) || !isActive())
	{
		return;
	}
	const bool on = !flightOn(unit);
	_flight[unit] = on;
	_landing = !on && unit->isFloating();
	std::fill(std::begin(_stepAllowed), std::end(_stepAllowed), -1);
	_state->warning(on ? "STR_FPS_FLIGHT_ON" : "STR_FPS_FLIGHT_OFF");
}

/**
 * Up or down a level: by grav lift, or flying with flight switched on. A flying soldier with
 * flight off keeps to the ground.
 */
void FirstPersonMode::moveUpDown(BattleUnit *unit, int direction, Action *action)
{
	if (!unit || !canCommit())
	{
		return;
	}
	const Tile *tile = _save->getTile(unit->getPosition());
	const bool gravLift = tile && tile->getMapData(O_FLOOR) && tile->getMapData(O_FLOOR)->isGravLift();
	if (canFly(unit) && !flightOn(unit) && !gravLift)
	{
		_state->warning("STR_FPS_FLIGHT_OFF");
		return;
	}
	if (direction == Pathfinding::DIR_UP)
	{
		_state->btnUnitUpClick(action);
	}
	else
	{
		_state->btnUnitDownClick(action);
	}
}

/**
 * Flight each frame: the soldier with flight on pays the flight cost setting per level (the
 * game's pathfinding asks us), and one switched off in the air comes down a level at a time.
 */
void FirstPersonMode::updateFlight(BattleUnit *unit)
{
	if (_landAfterStep && (!unit || !_stepPending))
	{
		_landAfterStep = false;
		_landing = unit && unit->isFloating() && !flightOn(unit);
	}
	if (_landing && (!unit || flightOn(unit) || !unit->isFloating()))
	{
		_landing = false;
	}
	const bool flies = unit && canFly(unit) && (flightOn(unit) || _landing);
	_save->getPathfinding()->setFlightCost(flies ? unit : nullptr, flies ? Options::fpsFlightCost : -1);
	if (_landing && canCommit() && unit->getStatus() == STATUS_STANDING && !_state->_battleGame->isBusy())
	{
		Pathfinding *pathfinding = _save->getPathfinding();
		const int cost = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_DOWN, unit, nullptr, BAM_NORMAL).cost.time;
		if (!pathfinding->validateUpDown(unit, unit->getPosition(), Pathfinding::DIR_DOWN))
		{
			_landing = false; // something below (another unit?): stay up
		}
		else if (cost > unit->getTimeUnits())
		{
			_landing = false; // out of time: hover here until the next turn
			_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		}
		else
		{
			_state->btnUnitDownClick(nullptr);
		}
	}
}

/**
 * The soldier card under a hovered squad button: name, wounds, weapon and the four stats,
 * and with the soldier preview option the live view from their eyes.
 */
void FirstPersonMode::updateCard()
{
	BattleUnit *unit = nullptr;
	if (_hoverIndex >= 0 && _hoverIndex < (int)_btnSquad.size() && _btnSquad[_hoverIndex]->getVisible() && !wantsMouseCapture())
	{
		unit = _squad[_hoverIndex];
	}
	const SquadState state = unit ? squadState(unit) : SQUAD_NONE;
	// the dead and the unconscious: their helmet camera shows only static
	const bool live = unit && unit->getTile() && (state == SQUAD_READY || state == SQUAD_CONTROLLED);
	if (state == SQUAD_NONE)
	{
		unit = nullptr;
	}
	const bool preview = unit && Options::fpsSoldierPreview;
	for (const auto &part : _cardParts)
	{
		part.surface->setVisible(unit && (part.surface != _preview || (preview && live)) && (part.surface != _cardSignal || (preview && !live)));
	}
	if (!unit)
	{
		return;
	}
	const TextButton *button = _btnSquad[_hoverIndex];
	const int x = std::clamp(button->getX() + button->getWidth() / 2 - CardW / 2, 1, canvasWidth() - CardW - 1);
	for (const auto &part : _cardParts)
	{
		part.surface->setX(x + part.dx);
		part.surface->setY(CardY + part.dy);
	}
	const int height = preview ? 36 + PreviewH + 4 : 35;
	if (height != _cardHeight)
	{
		_cardHeight = height;
		Uint8 background = 15, border = 8;
		if (const RuleInterface *ui = _game->getMod()->getInterface("firstPerson", false))
		{
			if (const Element *e = ui->getElementOptional("panel"))
			{
				if (e->color != INT_MAX) background = e->color;
				if (e->border != INT_MAX) border = e->border;
			}
		}
		_card->clear();
		_card->drawRect(0, 0, CardW, height, border);
		_card->drawRect(1, 1, CardW - 2, height - 2, background);
		if (preview)
		{
			_card->drawRect(3, 35, PreviewW + 2, PreviewH + 2, border); // frame around the camera
		}
	}
	_cardName->setText(unit->getName(_game->getLanguage(), false));
	_cardWeapon->setText(weaponLabel(unit));
	const std::string status = state == SQUAD_DEAD ? std::string(_state->tr("STR_FPS_DECEASED")) : state == SQUAD_UNCONSCIOUS ? std::string(_state->tr("STR_FPS_UNCONSCIOUS"))
		: state == SQUAD_CONTROLLED ? std::string(_state->tr("STR_FPS_MIND_CONTROLLED")) : statusLabel(unit);
	_cardStatus->setText(status);
	// the name gives way to the status beside it
	const int nameWidth = CardW - 8 - (status.empty() ? 0 : _cardStatus->getTextWidth() + 4);
	if (nameWidth != _cardName->getWidth())
	{
		_cardName->setWidth(nameWidth);
	}
	if (preview && !live)
	{
		// static on the dead camera
		const SDL_Color *pal = _state->getPalette();
		const Uint8 greys[4] = { nearestColor(pal, 20, 20, 20), nearestColor(pal, 70, 70, 70), nearestColor(pal, 130, 130, 130), nearestColor(pal, 200, 200, 200) };
		_card->lock();
		for (int ny = 0; ny < PreviewH; ++ny)
		{
			for (int nx = 0; nx < PreviewW; ++nx)
			{
				_noiseSeed = _noiseSeed * 1664525u + 1013904223u;
				_card->setPixel(4 + nx, 36 + ny, greys[(_noiseSeed >> 28) & 3]);
			}
		}
		_card->unlock();
	}
	const int values[4] = { unit->getTimeUnits(), unit->getEnergy(), unit->getHealth(), unit->getMorale() };
	const int maxima[4] = { unit->getBaseStats()->tu, unit->getBaseStats()->stamina, unit->getBaseStats()->health, 100 };
	for (int i = 0; i < 4; ++i)
	{
		_cardNumbers[i]->setValue(values[i]);
		_cardBars[i]->setMax(maxima[i]);
		_cardBars[i]->setValue(values[i]);
	}
	_cardBars[2]->setValue2(unit->getStunlevel());

	if (preview && live)
	{
		// their eyes: the 3D camera itself when it's the soldier we're looking through
		_preview->setCameraUnit(unit);
		if (isActive() && unit == _trackedUnit)
		{
			double eye[3];
			_view->getEye(eye);
			_preview->setEye(eye[0], eye[1], eye[2]);
			_preview->lookAlong(unit->getDirection());
			_preview->rotate(_view->getYawDegrees() - _preview->getYawDegrees(), 0.0);
		}
		else
		{
			const Position pos = unit->getPosition();
			const int size = unit->getArmor()->getSize();
			_preview->setEye(pos.x * 16.0 + 8.0 * size, pos.y * 16.0 + 8.0 * size, eyeHeight(unit));
			_preview->lookAlong(unit->getDirection());
		}
		_preview->setFieldOfView(Options::fpsFieldOfView);
		_preview->setSmoothLighting(Options::fpsSmoothLighting);
		_preview->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
		_preview->invalidate();
	}
}

/**
 * The surface the weapon in hand is drawn on: the whole canvas (made again when it changes size).
 */
void FirstPersonMode::makeViewModel()
{
	Surface *surface = new Surface(canvasWidth(), canvasHeight(), 0, 0);
	surface->setPalette(_state->getPalette());
	surface->setVisible(false);
	if (_viewModel)
	{
		std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), _viewModel, surface);
		_own.erase(_viewModel);
		delete _viewModel;
	}
	else
	{
		// right in front of the 3D view, behind everything else (a left-hand weapon never covers the panel)
		auto &surfaces = _state->_surfaces;
		surfaces.insert(std::find(surfaces.begin(), surfaces.end(), (Surface*)_view) + 1, surface);
	}
	_own.insert(surface);
	_viewModel = surface;
}

/**
 * Measures the weapon's inventory picture: its long axis (from where its pixels spread), the
 * muzzle at the thinner end (barrels are thin, stocks and grips aren't), and the grip and
 * magazine side (the one sticking out further) down.
 */
void FirstPersonMode::analyzeWeapon(bool gun)
{
	WeaponShape &shape = _vmShape;
	shape = WeaponShape();
	shape.gun = gun;
	std::vector<std::pair<int, int>> pixels;
	for (int y = 0; y < _handSprite->getHeight(); ++y)
		for (int x = 0; x < _handSprite->getWidth(); ++x)
			if (_handSprite->getPixel(x, y)) pixels.push_back({ x, y });
	if (pixels.size() < 8)
	{
		shape.gun = false;
		return;
	}
	double mx = 0, my = 0;
	for (const auto &p : pixels) { mx += p.first; my += p.second; }
	mx /= pixels.size(); my /= pixels.size();
	double cxx = 0, cyy = 0, cxy = 0;
	for (const auto &p : pixels)
	{
		const double dx = p.first - mx, dy = p.second - my;
		cxx += dx * dx; cyy += dy * dy; cxy += dx * dy;
	}
	const double theta = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
	shape.mean[0] = mx; shape.mean[1] = my;
	shape.along[0] = std::cos(theta); shape.along[1] = std::sin(theta);
	shape.across[0] = -std::sin(theta); shape.across[1] = std::cos(theta);
	double min1 = 1e9, max1 = -1e9, min2 = 1e9, max2 = -1e9;
	for (const auto &p : pixels)
	{
		const double dx = p.first - mx, dy = p.second - my;
		const double u = dx * shape.along[0] + dy * shape.along[1], v = dx * shape.across[0] + dy * shape.across[1];
		min1 = std::min(min1, u); max1 = std::max(max1, u); min2 = std::min(min2, v); max2 = std::max(max2, v);
	}
	// how thick each end is (the quarter of the length at each end)
	double lowMin = 1e9, lowMax = -1e9, highMin = 1e9, highMax = -1e9;
	const double quarter = (max1 - min1) * 0.25;
	for (const auto &p : pixels)
	{
		const double dx = p.first - mx, dy = p.second - my;
		const double u = dx * shape.along[0] + dy * shape.along[1], v = dx * shape.across[0] + dy * shape.across[1];
		if (u < min1 + quarter) { lowMin = std::min(lowMin, v); lowMax = std::max(lowMax, v); }
		if (u > max1 - quarter) { highMin = std::min(highMin, v); highMax = std::max(highMax, v); }
	}
	const double lowSpread = lowMax - lowMin, highSpread = highMax - highMin;
	bool muzzleHigh;
	if (highSpread < lowSpread - 1.0) muzzleHigh = true;
	else if (lowSpread < highSpread - 1.0) muzzleHigh = false;
	else muzzleHigh = std::abs(shape.along[1]) > std::abs(shape.along[0]) ? shape.along[1] < 0 : shape.along[0] < 0; // even: the end nearer the top (or left)
	shape.stock = muzzleHigh ? min1 : max1;
	shape.muzzle = muzzleHigh ? max1 : min1;
	shape.minAcross = min2;
	shape.maxAcross = max2;
	shape.upSign = max2 > -min2 ? -1 : 1;
	// the bore: the middle of the last two pixels' worth at the muzzle end, which goes on the
	// ray to the crosshair (the shot leaves from there), not the middle of the whole weapon
	double boreSum = 0.0;
	int boreCount = 0;
	for (const auto &p : pixels)
	{
		const double dx = p.first - mx, dy = p.second - my;
		const double u = dx * shape.along[0] + dy * shape.along[1], v = dx * shape.across[0] + dy * shape.across[1];
		if (std::abs(u - shape.muzzle) <= 2.0) { boreSum += v; ++boreCount; }
	}
	shape.bore = boreCount ? boreSum / boreCount : 0.0;
	voxelizeWeapon();
}

/**
 * Gives the weapon's picture volume, cushion style: each pixel's half depth grows with the
 * square root of its distance to the outline (one voxel at the edge, at most four), and only
 * the faces between a voxel and empty space are kept.
 */
void FirstPersonMode::voxelizeWeapon()
{
	_vmFaces.clear();
	if (!_vmShape.gun)
	{
		return;
	}
	const int w = _handSprite->getWidth(), h = _handSprite->getHeight();
	const int MaxHalf = 4, depth = 2 * MaxHalf + 1;
	// steps to the outside (8 neighbours), a breadth-first walk in from the edge pixels
	std::vector<int> dist(w * h, 0);
	std::vector<int> queue;
	auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && _handSprite->getPixel(x, y) != 0; };
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			if (!solid(x, y)) continue;
			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy)
				for (int dx = -1; dx <= 1 && !edge; ++dx)
					edge = !solid(x + dx, y + dy);
			if (edge)
			{
				dist[y * w + x] = 1;
				queue.push_back(y * w + x);
			}
		}
	}
	for (size_t head = 0; head < queue.size(); ++head)
	{
		const int x = queue[head] % w, y = queue[head] / w;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
				if (solid(x + dx, y + dy) && !dist[(y + dy) * w + x + dx])
				{
					dist[(y + dy) * w + x + dx] = dist[queue[head]] + 1;
					queue.push_back((y + dy) * w + x + dx);
				}
	}
	std::vector<Uint8> cells(w * h * depth, 0);
	auto cell = [&](int x, int y, int z) -> Uint8 {
		return x >= 0 && y >= 0 && z >= 0 && x < w && y < h && z < depth ? cells[(z * h + y) * w + x] : 0;
	};
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			if (!dist[y * w + x]) continue;
			const int half = std::clamp((int)std::lround(1.2 * std::sqrt((double)dist[y * w + x])), 1, MaxHalf);
			for (int z = MaxHalf - half; z <= MaxHalf + half; ++z) cells[(z * h + y) * w + x] = _handSprite->getPixel(x, y);
		}
	}
	// a face for every side of a voxel open to the air, its corners wound so that the cross
	// product of its first two edges points outwards
	for (int z = 0; z < depth; ++z)
	{
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const Uint8 color = cell(x, y, z);
				if (!color) continue;
				const int p[3] = { x, y, z };
				for (int axis = 0; axis < 3; ++axis)
				{
					for (int dir = -1; dir <= 1; dir += 2)
					{
						int n[3] = { x, y, z };
						n[axis] += dir;
						if (cell(n[0], n[1], n[2])) continue;
						const int t1 = dir > 0 ? (axis + 1) % 3 : (axis + 2) % 3, t2 = dir > 0 ? (axis + 2) % 3 : (axis + 1) % 3;
						VoxelFace face;
						face.color = color;
						if (axis < 2)
						{
							// the picture's dark outline stays on its face; the edges show the colour inside it
							const int ix = x - (axis == 0 ? dir : 0), iy = y - (axis == 1 ? dir : 0);
							if (solid(ix, iy)) face.color = _handSprite->getPixel(ix, iy);
						}
						for (int k = 0; k < 4; ++k)
						{
							float c[3] = { p[0] - 0.5f, p[1] - 0.5f, p[2] - 0.5f - MaxHalf };
							c[axis] += dir > 0 ? 1.0f : 0.0f;
							if (k == 1 || k == 2) c[t1] += 1.0f;
							if (k == 2 || k == 3) c[t2] += 1.0f;
							std::copy(c, c + 3, face.corner[k]);
						}
						_vmFaces.push_back(face);
					}
				}
			}
		}
	}
}

/**
 * How the muzzle flashes, by the damage the weapon's ammo does: a yellow star for bullets, a big
 * orange ball for explosives, red-orange for fire, thin red spikes for lasers (cyan under the
 * sea), a round green glow for plasma, blue for stun, yellow-green for acid; bigger the harder
 * it hits.
 */
FirstPersonMode::MuzzleFlash FirstPersonMode::flashFor(const BattleItem *weapon) const
{
	// what the shot fired (a launcher's only rocket is gone by now), else what's loaded
	const BattleItem *ammo = weapon->getAmmoForSlot(0);
	const RuleItem *rules = _view->getOwnShotAmmo() ? _view->getOwnShotAmmo() : ammo ? ammo->getRules() : weapon->getRules();
	const RuleDamageType *damage = rules->getDamageType();
	const bool underwater = _save->getDepth() > 0;
	const SDL_Color *pal = _state->getPalette();
	struct Look { int core[3], mid[3], rim[3]; int rays; double spike, sharpness, size; };
	Look look = { { 255, 250, 210 }, { 255, 200, 60 }, { 240, 110, 20 }, 4, 1.1, 1.5, 1.5 }; // bullets
	switch (damage ? damage->ResistType : DT_AP)
	{
	case DT_HE: look = { { 255, 240, 190 }, { 255, 150, 40 }, { 190, 60, 15 }, 8, 0.4, 1.0, 1.5 }; break;
	case DT_IN: look = { { 255, 220, 150 }, { 255, 110, 30 }, { 180, 30, 10 }, 6, 0.45, 1.0, 1.5 }; break;
	case DT_LASER:
		look = underwater ? Look{ { 255, 255, 255 }, { 120, 230, 255 }, { 40, 140, 220 }, 8, 1.2, 6.0, 0.9 }
			: Look{ { 255, 255, 255 }, { 255, 90, 70 }, { 200, 20, 20 }, 8, 1.2, 6.0, 0.9 };
		break;
	case DT_PLASMA:
		look = underwater ? Look{ { 240, 255, 255 }, { 190, 240, 255 }, { 60, 150, 200 }, 5, 0.15, 1.0, 1.4 }
			: Look{ { 230, 255, 220 }, { 110, 255, 110 }, { 30, 170, 60 }, 5, 0.15, 1.0, 1.4 };
		break;
	case DT_STUN: look = { { 230, 240, 255 }, { 150, 180, 255 }, { 70, 90, 220 }, 6, 0.6, 2.0, 1.3 }; break;
	case DT_ACID: look = { { 240, 255, 200 }, { 190, 255, 60 }, { 90, 170, 20 }, 5, 0.3, 1.0, 1.4 }; break;
	default: break;
	}
	MuzzleFlash flash;
	flash.core = nearestColor(pal, look.core[0], look.core[1], look.core[2]);
	flash.mid = nearestColor(pal, look.mid[0], look.mid[1], look.mid[2]);
	flash.rim = nearestColor(pal, look.rim[0], look.rim[1], look.rim[2]);
	flash.rays = look.rays;
	flash.spike = look.spike;
	flash.sharpness = look.sharpness;
	flash.size = look.size * std::clamp(rules->getPower() / 50.0, 0.8, 1.5);
	return flash;
}

/**
 * The voxel weapon: placed exactly like the flat plate (picture x, y mapped onto the plate,
 * depth along its normal at the plate's across scale), the faces that look at the eye drawn
 * far to near, each darkened by how it turns from the light.
 */
void FirstPersonMode::drawWeaponVoxels(const double nearEnd[3], const double along[3], const double up[3], const double normal[3], double perPixel, int shade)
{
	const WeaponShape &shape = _vmShape;
	const double span = shape.muzzle - shape.stock;
	// camera = origin + x * ax + y * ay + z * az (the plate mapping, see drawWeaponPlate)
	double ax[3], ay[3], az[3], origin[3];
	for (int a = 0; a < 3; ++a)
	{
		const double alongUnit = along[a] / span, upUnit = shape.upSign * perPixel * up[a];
		ax[a] = shape.along[0] * alongUnit + shape.across[0] * upUnit;
		ay[a] = shape.along[1] * alongUnit + shape.across[1] * upUnit;
		az[a] = perPixel * normal[a];
		const double u0 = -shape.mean[0] * shape.along[0] - shape.mean[1] * shape.along[1];
		const double v0 = -shape.mean[0] * shape.across[0] - shape.mean[1] * shape.across[1];
		origin[a] = nearEnd[a] + (u0 - shape.stock) * alongUnit + (v0 - shape.bore) * upUnit;
	}
	// a mirror image (the left hand) turns the windings round
	const double det = ax[0] * (ay[1] * az[2] - ay[2] * az[1]) - ax[1] * (ay[0] * az[2] - ay[2] * az[0]) + ax[2] * (ay[0] * az[1] - ay[1] * az[0]);
	const double flip = det < 0.0 ? -1.0 : 1.0;
	const double light[3] = { -0.42, -0.78, -0.46 }; // towards the light: up, left, behind the eye
	const int w = _viewModel->getWidth(), h = _viewModel->getHeight();
	const double focal = w / 2.0 * WeaponLens, cx = w / 2.0, cy = h / 2.0;
	struct Quad { double depth; double screen[4][2]; Uint8 color; };
	std::vector<Quad> quads;
	quads.reserve(_vmFaces.size() / 2);
	for (const VoxelFace &face : _vmFaces)
	{
		double cam[4][3];
		for (int k = 0; k < 4; ++k)
			for (int a = 0; a < 3; ++a)
				cam[k][a] = origin[a] + face.corner[k][0] * ax[a] + face.corner[k][1] * ay[a] + face.corner[k][2] * az[a];
		double e1[3], e2[3], n[3], centre[3];
		for (int a = 0; a < 3; ++a)
		{
			e1[a] = cam[1][a] - cam[0][a];
			e2[a] = cam[3][a] - cam[0][a];
			centre[a] = (cam[0][a] + cam[2][a]) * 0.5;
		}
		n[0] = (e1[1] * e2[2] - e1[2] * e2[1]) * flip;
		n[1] = (e1[2] * e2[0] - e1[0] * e2[2]) * flip;
		n[2] = (e1[0] * e2[1] - e1[1] * e2[0]) * flip;
		if (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] >= 0.0 || centre[2] < 0.1)
		{
			continue; // turned away, or behind the eye
		}
		const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		const double lit = std::max(0.0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl);
		const int darker = (int)std::lround((1.0 - lit) * 3.0) + shade;
		Quad quad;
		quad.depth = centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2];
		quad.color = (Uint8)((face.color & 0xF0) | std::min(15, (face.color & 15) + darker));
		for (int k = 0; k < 4; ++k)
		{
			quad.screen[k][0] = cx + cam[k][0] / cam[k][2] * focal;
			quad.screen[k][1] = cy + cam[k][1] / cam[k][2] * focal;
		}
		quads.push_back(quad);
	}
	std::sort(quads.begin(), quads.end(), [](const Quad &a, const Quad &b) { return a.depth > b.depth; });
	for (const Quad &quad : quads)
	{
		fillQuad(quad.screen, quad.color);
	}
}

/**
 * Fills a convex quadrilateral on the weapon surface (pixel centres inside it).
 */
void FirstPersonMode::fillQuad(const double corners[4][2], Uint8 color)
{
	const int w = _viewModel->getWidth(), h = _viewModel->getHeight();
	double top = corners[0][1], bottom = corners[0][1];
	for (int k = 1; k < 4; ++k)
	{
		top = std::min(top, corners[k][1]);
		bottom = std::max(bottom, corners[k][1]);
	}
	const int y0 = std::max(0, (int)std::ceil(top - 0.5)), y1 = std::min(h - 1, (int)std::floor(bottom - 0.5));
	for (int y = y0; y <= y1; ++y)
	{
		const double py = y + 0.5;
		double left = 1e9, right = -1e9;
		for (int k = 0; k < 4; ++k)
		{
			const double *a = corners[k], *b = corners[(k + 1) % 4];
			if ((a[1] <= py && b[1] > py) || (b[1] <= py && a[1] > py))
			{
				const double x = a[0] + (py - a[1]) / (b[1] - a[1]) * (b[0] - a[0]);
				left = std::min(left, x);
				right = std::max(right, x);
			}
		}
		const int x0 = std::max(0, (int)std::ceil(left - 0.5)), x1 = std::min(w - 1, (int)std::floor(right - 0.5));
		for (int x = x0; x <= x1; ++x)
		{
			_viewModel->setPixel(x, y, color);
		}
	}
}

/**
 * The weapon in hand, Doom style: its picture on a plate in perspective, the muzzle reaching
 * in under the crosshair and the rest running off the bottom corner (right or left, by the
 * hand), with a little depth. It bobs with the steps, kicks back and up when it fires, and
 * dips out of sight and back on a reload or a change of weapon.
 */
void FirstPersonMode::drawWeaponPlate(bool left, double bob[2], double kick, double dip, int shade, double tip[2])
{
	const WeaponShape &shape = _vmShape;
	const int w = _viewModel->getWidth(), h = _viewModel->getHeight();
	const double focal = w / 2.0 * WeaponLens, cx = w / 2.0, cy = h / 2.0;
	const double side = left ? -1.0 : 1.0;
	// camera space: x right, y down, z ahead
	// The muzzle is always at the same point on the screen, down and out towards the corner
	// (MuzzleScreen; the shot's flash and trail start there). The stock is on the same line from
	// the crosshair, 2.5 times further out (off the bottom corner), so the barrel points along a
	// ray at the crosshair whatever the weapon.
	// a melee swing pulls the weapon back and out, then drives it in towards the crosshair
	double windup = 0.0, strike = 0.0;
	swingPhase(windup, strike);
	const double reachFar = 1.0 + 0.25 * windup - 0.85 * strike, reachNear = 1.0 + 0.1 * windup - 0.3 * strike;
	const double zFar = (6.9 - kick * 0.65) * (1.0 - 0.3 * strike), zNear = (1.95 - kick * 0.22) * (1.0 - 0.15 * strike); // well back: mostly hidden
	const double sway[2] = { bob[0] * 0.002, bob[1] * 0.002 + dip * 0.004 };
	double farEnd[3] = { side * (MuzzleScreen[0] * reachFar + sway[0]) * zFar, (MuzzleScreen[1] * reachFar + sway[1] - kick * 0.03 - 0.05 * windup) * zFar, zFar };
	double nearEnd[3] = { side * (MuzzleScreen[0] * 2.5 * reachNear + sway[0]) * zNear, (MuzzleScreen[1] * 2.5 * reachNear + sway[1]) * zNear, zNear };
	// the weapon a little shorter than that line, towards the muzzle (still on the same ray)
	for (int a = 0; a < 3; ++a) nearEnd[a] = farEnd[a] + (nearEnd[a] - farEnd[a]) * WeaponScale;
	// a shorter weapon than the rifle shrinks about its grip (still on the line at the crosshair)
	const double fit = std::pow(std::clamp(std::abs(shape.muzzle - shape.stock) / RiflePixels, 0.4, 1.0), SizeCurve);
	for (int a = 0; a < 3; ++a)
	{
		const double pivot = farEnd[a] + (nearEnd[a] - farEnd[a]) * GripAt;
		farEnd[a] = pivot + (farEnd[a] - pivot) * fit;
		nearEnd[a] = pivot + (nearEnd[a] - pivot) * fit;
	}
	double along[3], length = 0.0;
	for (int a = 0; a < 3; ++a) { along[a] = farEnd[a] - nearEnd[a]; length += along[a] * along[a]; }
	length = std::sqrt(length);
	const double pixelsLong = std::max(4.0, std::abs(shape.muzzle - shape.stock));
	// across (and in depth) the same scale as along: the weapon keeps its proportions
	const double perPixel = length / pixelsLong;
	// up across the plate: straight up, made square to its length
	double up[3] = { 0.0, -1.0, 0.0 };
	const double d = (up[0] * along[0] + up[1] * along[1] + up[2] * along[2]) / (length * length);
	for (int a = 0; a < 3; ++a) up[a] -= along[a] * d;
	const double upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	for (int a = 0; a < 3; ++a) up[a] /= upLength;
	double normal[3] = { along[1] * up[2] - along[2] * up[1], along[2] * up[0] - along[0] * up[2], along[0] * up[1] - along[1] * up[0] };
	const double normalLength = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	for (int a = 0; a < 3; ++a) normal[a] /= normalLength;
	// the tip on screen, for the muzzle flash
	tip[0] = cx + farEnd[0] / farEnd[2] * focal;
	tip[1] = cy + farEnd[1] / farEnd[2] * focal;
	// the plate's corners on screen bound the pixels to fill
	const double reach = std::max(std::abs(shape.minAcross - shape.bore), std::abs(shape.maxAcross - shape.bore)) * perPixel + 0.1;
	int x0 = w, x1 = -1, y0 = h, y1 = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double *end = corner < 2 ? nearEnd : farEnd;
		const double off = (corner & 1) ? reach : -reach;
		const double p[3] = { end[0] + up[0] * off, end[1] + up[1] * off, std::max(0.2, end[2] + up[2] * off) };
		const double sx = cx + p[0] / p[2] * focal, sy = cy + p[1] / p[2] * focal;
		x0 = std::min(x0, (int)sx - 1); x1 = std::max(x1, (int)sx + 1);
		y0 = std::min(y0, (int)sy - 1); y1 = std::max(y1, (int)sy + 1);
	}
	x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
	if (Options::fpsVoxelWeapon && !_vmFaces.empty())
	{
		drawWeaponVoxels(nearEnd, along, up, normal, perPixel, shade);
		return;
	}
	// two layers: the far face a little darker shows round the edges, for some body
	const double thickness = 2.5 * perPixel;
	for (int layer = 0; layer < 2; ++layer)
	{
		const double offset = layer == 0 ? thickness * side : 0.0;
		const double origin[3] = { nearEnd[0] + normal[0] * offset, nearEnd[1] + normal[1] * offset, nearEnd[2] + normal[2] * offset };
		const double planeD = origin[0] * normal[0] + origin[1] * normal[1] + origin[2] * normal[2];
		const int layerShade = std::min(15, shade + (layer == 0 ? 3 : 0));
		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const double ray[3] = { (x + 0.5 - cx) / focal, (y + 0.5 - cy) / focal, 1.0 };
				const double denom = ray[0] * normal[0] + ray[1] * normal[1] + ray[2] * normal[2];
				if (std::abs(denom) < 1e-6) continue;
				const double t = planeD / denom;
				if (t <= 0.0) continue;
				const double rel[3] = { ray[0] * t - origin[0], ray[1] * t - origin[1], ray[2] * t - origin[2] };
				const double a = (rel[0] * along[0] + rel[1] * along[1] + rel[2] * along[2]) / (length * length);
				if (a < 0.0 || a > 1.0) continue;
				const double b = (rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) / perPixel;
				const double u = shape.stock + a * (shape.muzzle - shape.stock), v = shape.upSign * b + shape.bore;
				if (v < shape.minAcross - 0.5 || v > shape.maxAcross + 0.5) continue;
				const int sx = (int)std::floor(shape.mean[0] + u * shape.along[0] + v * shape.across[0] + 0.5);
				const int sy = (int)std::floor(shape.mean[1] + u * shape.along[1] + v * shape.across[1] + 0.5);
				if (sx < 0 || sy < 0 || sx >= _handSprite->getWidth() || sy >= _handSprite->getHeight()) continue;
				const Uint8 color = _handSprite->getPixel(sx, sy);
				if (color) _viewModel->setPixel(x, y, (Uint8)((color & 0xF0) | std::min(15, (color & 15) + layerShade)));
			}
		}
	}
}

/**
 * Anything that isn't a gun or a club (grenade, medi-kit, scanner...): held up in the corner,
 * half below the edge of the screen.
 */
void FirstPersonMode::drawHeldItem(bool left, double bob[2], double dip, int shade)
{
	const int scale = 3, w = _viewModel->getWidth(), h = _viewModel->getHeight();
	const int sw = _handSprite->getWidth(), sh = _handSprite->getHeight();
	const int x0 = (left ? PanelW + 10 : w - sw * scale - 12) + (int)bob[0]; // the left hand's clear of the info panel
	const int y0 = h - sh * scale * 6 / 10 + (int)(bob[1] + dip);
	for (int y = 0; y < sh * scale; ++y)
	{
		for (int x = 0; x < sw * scale; ++x)
		{
			const Uint8 color = _handSprite->getPixel(x / scale, y / scale);
			if (color && x0 + x >= 0 && y0 + y >= 0 && x0 + x < w && y0 + y < h)
				_viewModel->setPixel(x0 + x, y0 + y, (Uint8)((color & 0xF0) | std::min(15, (color & 15) + shade)));
		}
	}
}

/**
 * The weapon in the soldier's hand, at the bottom of the screen like an FPS (option "Weapon
 * in hand"), shaded by the light around the soldier.
 */
void FirstPersonMode::updateViewModel(BattleUnit *unit)
{
	BattleItem *weapon = unit ? activeWeapon(unit) : nullptr;
	SurfaceSet *sprites = _game->getMod()->getSurfaceSet("BIGOBS.PCK");
	const bool show = Options::fpsViewModel && isActive() && !_classicHud && weapon && !_deathUnit && sprites;
	_viewModel->setVisible(show);
	_view->setMuzzlePoint(0.0, 0.0, false); // set again below while a gun is in hand
	_view->setOffMuzzlePoint(0.0, 0.0, false);
	if (!show)
	{
		_vmItem = nullptr;
		return;
	}
	const Uint32 now = SDL_GetTicks();
	const BattleItem *ammo = weapon->getAmmoForSlot(0);
	const int rounds = ammo && ammo != weapon ? ammo->getAmmoQuantity() : -1;
	if (weapon != _vmItem || (rounds > _vmAmmo && _vmAmmo >= 0))
	{
		_vmDipStart = _vmItem ? now : 0; // a reload or another weapon: dip out and back (not on the first frame)
	}
	_vmItem = weapon;
	_vmAmmo = rounds;
	const BattleType type = weapon->getRules()->getBattleType();
	if (weapon->getRules() != _vmRules)
	{
		_vmRules = weapon->getRules();
		_handSprite->clear();
		weapon->getRules()->drawHandSprite(sprites, _handSprite, weapon, _save, 0);
		analyzeWeapon(type == BT_FIREARM || type == BT_MELEE);
	}
	const int shade = std::min(9, _view->getEyeShade());
	const bool left = weapon == unit->getLeftHandWeapon();
	BattleItem *other = offHand(unit, weapon); // dual wield: the other hand's gun, on the other side
	// bob with the steps, kick when it fires, dip on a reload or a change of weapon
	const double step = _bobPhase * 2.0 * Pi / 16.0;
	double bob[2] = { std::sin(step) * 4.0 * _bobAmount, std::abs(std::cos(step)) * 4.0 * _bobAmount };
	if (_view->getOwnShotTick() != _vmShotSeen)
	{
		_vmShotSeen = _view->getOwnShotTick();
		if (type == BT_FIREARM)
		{
			_vmFlashUntil = now + 80;
			_vmFlash = flashFor(weapon);
		}
	}
	if (_view->getPartnerShotTick() != _offShotSeen)
	{
		_offShotSeen = _view->getPartnerShotTick(); // dual wield: the other gun fired with it
		if (other)
		{
			_offFlashUntil = now + 80;
			_offFlash = flashFor(other);
		}
	}
	const double kick = std::min(1.0, _view->getKick() / 4.0);
	double dip = 0.0;
	if (_vmDipStart && now - _vmDipStart < 600)
	{
		dip = std::sin(Pi * (now - _vmDipStart) / 600.0) * 70.0;
	}
	_viewModel->clear();
	_viewModel->lock();
	// a muzzle flash at the tip, behind the weapon (with the muzzle flashes setting)
	auto drawFlash = [&](const double tip[2], const MuzzleFlash &flash)
	{
		const int r = std::max(5, (int)std::lround(_viewModel->getWidth() / 30.0 * flash.size));
		const int reach = (int)std::ceil(r * (1.0 + flash.spike));
		// centred just ahead of the bore, along the barrel towards the crosshair: the muzzle
		// covers the back of it
		const double toX = _viewModel->getWidth() / 2.0 - tip[0], toY = _viewModel->getHeight() / 2.0 - tip[1];
		const double toLength = std::max(1.0, std::sqrt(toX * toX + toY * toY));
		const int fx = (int)std::lround(tip[0] + toX / toLength * r * 0.5), fy = (int)std::lround(tip[1] + toY / toLength * r * 0.5);
		const Uint8 core = flash.core, mid = flash.mid, rim = flash.rim;
		for (int y = -reach; y <= reach; ++y)
		{
			for (int x = -reach; x <= reach; ++x)
			{
				const double dist = std::sqrt((double)(x * x + y * y)), angle = std::atan2((double)y, (double)x);
				const double ray = std::pow(std::max(0.0, std::cos(angle * flash.rays + now * 0.05)), flash.sharpness);
				const double edge = r * (1.0 + flash.spike * ray);
				if (dist > edge) continue;
				const int px = fx + x, py = fy + y;
				if (px < 0 || py < 0 || px >= _viewModel->getWidth() || py >= _viewModel->getHeight()) continue;
				if (_viewModel->getPixel(px, py)) continue; // behind the weapon: it shows round the muzzle
				_viewModel->setPixel(px, py, dist < edge * 0.35 ? core : dist < edge * 0.7 ? mid : rim);
			}
		}
	};
	if (_vmShape.gun)
	{
		double tip[2];
		drawWeaponPlate(left, bob, kick, dip, shade, tip);
		// our shots leave this muzzle (dual wield: the other gun's rounds the one on the other side)
		const double toView = _view->getWidth() / (double)_viewModel->getWidth();
		_view->setMuzzlePoint(tip[0] * toView, tip[1] * toView, true);
		if (Options::fpsMuzzleFlash && now < _vmFlashUntil)
		{
			drawFlash(tip, _vmFlash);
		}
		if (other)
		{
			swapOffHand();
			if (other->getRules() != _vmRules)
			{
				_vmRules = other->getRules();
				_handSprite->clear();
				other->getRules()->drawHandSprite(sprites, _handSprite, other, _save, 0);
				analyzeWeapon(true);
			}
			if (_vmShape.gun)
			{
				double otherTip[2];
				drawWeaponPlate(!left, bob, kick, dip, shade, otherTip);
				_view->setOffMuzzlePoint(otherTip[0] * toView, otherTip[1] * toView, true);
				if (Options::fpsMuzzleFlash && now < _offFlashUntil)
				{
					drawFlash(otherTip, _offFlash);
				}
			}
			swapOffHand();
		}
	}
	else
	{
		drawHeldItem(left, bob, dip, shade);
	}
	_viewModel->unlock();
}

/**
 * Tosses a tactical flare at the crosshair (free and unlimited, with the tactical flares
 * setting): the 3D view flies it, bounces it and lets it burn.
 */
void FirstPersonMode::throwFlare()
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!Options::fpsFlares || !unit || !isActive() || _deathUnit)
	{
		return;
	}
	Position voxel;
	aimAt(_view->getWidth() / 2, _view->getHeight() / 2, unit, voxel); // a point far along the ray if nothing's hit
	const float target[3] = { (float)voxel.x, (float)voxel.y, (float)voxel.z };
	_view->throwFlare(target);
	if (Sound *sound = _game->getMod()->getSoundByDepth(_save->getDepth(), Mod::ITEM_THROW))
	{
		sound->play(-1, _state->_map->getSoundAngle(unit->getPosition()));
	}
	_sounds->play(FirstPersonSounds::FLARE, 0, 0.55f); // struck and hissing
}

/**
 * Opens the door the soldier faces without walking through it, like right-clicking it on
 * the map: a turn on the spot that the turning state turns into opening the door.
 */
void FirstPersonMode::openDoor(BattleUnit *unit)
{
	if (!unit || !canCommit() || unit->getStatus() != STATUS_STANDING)
	{
		return;
	}
	Position ahead;
	Pathfinding::directionToVector(unit->getDirection(), &ahead);
	BattleAction turn;
	turn.type = BA_NONE;
	turn.actor = unit;
	turn.target = unit->getPosition() + ahead;
	_state->_battleGame->statePushBack(new UnitTurnBState(_state->_battleGame, turn));
}

/**
 * "Hidden movement" while it's not your turn and no enemy is in sight.
 */
void FirstPersonMode::updateBanner(bool playerTurn)
{
	std::string text;
	if (isActive() && playerTurn && SDL_GetTicks() < _spottedUntil)
	{
		text = _state->tr("STR_FPS_ENEMY_SPOTTED");
	}
	else if (isActive() && !playerTurn)
	{
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
	if (!text.empty() && _txtBanner->getText() != text)
	{
		_txtBanner->setText(text);
		for (Text *glow : _bannerGlow)
		{
			glow->setText(text);
		}
	}
	_txtBanner->setVisible(!text.empty());
	for (Text *glow : _bannerGlow)
	{
		glow->setVisible(!text.empty());
	}
	if (text.empty())
	{
		return;
	}
	// glowing letters: a yellow core that pulses towards orange, in a halo going red to purple
	// (enemy spotted pulses faster, in plain red)
	const SDL_Color *pal = _game->getScreen()->getPalette();
	const bool alarm = playerTurn;
	const double t = SDL_GetTicks() / (alarm ? 160.0 : 450.0);
	const double pulse = 0.5 + 0.5 * std::sin(t);
	const Uint8 core = pulse > 0.5 ? textColor(pal, 255, 230, 70) : textColor(pal, 255, 170, 40);
	const Uint8 halo = alarm ? textColor(pal, 240, 40, 30)
		: (std::sin(t * 0.37) > 0 ? textColor(pal, 235, 50, 40) : textColor(pal, 190, 70, 230));
	if (_txtBanner->getColor() != core)
	{
		_txtBanner->setColor(core);
	}
	for (Text *glow : _bannerGlow)
	{
		if (glow->getColor() != halo) glow->setColor(halo);
	}
	if (_shadows.front()->getColor() != halo) _shadows.front()->setColor(halo); // the banner's shadow is part of the halo
}

/**
 * Aims an action on the isometric map: a guided missile's waypoints, or a psi attack or
 * mind probe on a creature out of sight. The view switches there with the action already
 * being aimed; the game's own clicks (and launch button) take over, and once the action is
 * done (or called off) the view comes back to 3D.
 */
void FirstPersonMode::startMapAim(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const char *hint)
{
	BattlescapeGame *battleGame = _state->_battleGame;
	BattleAction *action = battleGame->getCurrentAction();
	action->actor = unit;
	action->weapon = weapon;
	action->type = mode;
	action->targeting = true;
	action->waypoints.clear();
	action->updateTU();
	battleGame->setupCursor();
	_mapAim = MAPAIM_AIMING;
	_mapAimType = mode;
	_wanted = false;
	remember();
	think();
	_state->warning(hint);
}

/**
 * Follows an action aimed on the isometric map; back to 3D when it's done or called off.
 */
void FirstPersonMode::updateMapAim()
{
	if (_mapAim == MAPAIM_NONE)
	{
		return;
	}
	const BattleAction *action = _state->_battleGame->getCurrentAction();
	const bool busy = _state->_battleGame->isBusy();
	bool done = false;
	if (_mapAim == MAPAIM_AIMING)
	{
		if (busy)
		{
			_mapAim = MAPAIM_ACTING; // fired: the missile flies, the psi attack happens
		}
		else if (!(action->targeting && action->type == _mapAimType))
		{
			done = true; // done at once (the mind probe's report) or called off
		}
	}
	else if (!busy && _state->_popups.empty())
	{
		done = true;
	}
	if (done)
	{
		_mapAim = MAPAIM_NONE;
		if (!_wanted)
		{
			_wanted = true;
			remember();
		}
	}
}

/**
 * Is there a creature (other than the soldier) the player can see at this point?
 */
bool FirstPersonMode::creatureAt(const Position &voxel, const BattleUnit *self) const
{
	for (int dz = 0; dz >= -1; --dz)
	{
		const Tile *tile = _save->getTile(voxel.toTile() + Position(0, 0, dz));
		const BattleUnit *unit = tile ? tile->getUnit() : nullptr;
		if (unit && unit != self && !unit->isOut() && unit->getVisible())
		{
			return true;
		}
	}
	return false;
}

/**
 * Combat music: while aliens are in sight a tenser track plays; when none has been seen for
 * a few seconds the battle music comes back.
 */
void FirstPersonMode::updateMusic(Uint32 now)
{
	Mod *mod = _game->getMod();
	auto battleMusic = [&]() { mod->playMusic(_save->getMusic().empty() ? std::string("GMTACTIC") : _save->getMusic()); };
	if (!Options::fpsCombatMusic)
	{
		if (_musicTense)
		{
			_musicTense = false;
			battleMusic();
		}
		return;
	}
	bool seen = false;
	for (const BattleUnit *unit : *_save->getUnits())
	{
		if (!unit->isOut() && unit->getFaction() == FACTION_HOSTILE && unit->getVisible())
		{
			seen = true;
			break;
		}
	}
	if (seen)
	{
		_lastEnemySeen = now;
	}
	if (seen && !_musicTense)
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
	else if (!seen && _musicTense && now - _lastEnemySeen > 8000)
	{
		_musicTense = false;
		battleMusic();
	}
}

/**
 * The soldier we look through got hurt: flash red, more for a bigger wound.
 */
void FirstPersonMode::checkHurt(BattleUnit *unit, Uint32 now)
{
	if (unit != _healthUnit)
	{
		_healthUnit = unit;
		_lastHealth = unit->getHealth();
		return;
	}
	const int health = unit->getHealth();
	if (health < _lastHealth)
	{
		_hurtStart = now;
		_hurtAmount = std::clamp((_lastHealth - health) / 12.0f + 0.3f, 0.3f, 0.85f);
		_view->addShake(1.0f + _hurtAmount * 2.0f);
		// where it came from: the enemy shot that just ended, or an alien right beside us (melee:
		// a hard jolt and blood over the view)
		double from[3];
		const Uint32 shotEnd = _view->getEnemyShotEnd(from);
		const bool shot = shotEnd && now - shotEnd < 800;
		const BattleUnit *attacker = nullptr;
		if (!shot)
		{
			const Position at = unit->getPosition();
			for (const BattleUnit *other : *_save->getUnits())
			{
				const Position o = other->getPosition();
				if (!other->isOut() && other->getFaction() == FACTION_HOSTILE && o.z == at.z && std::abs(o.x - at.x) <= 1 && std::abs(o.y - at.y) <= 1)
				{
					attacker = other;
				}
			}
			if (attacker)
			{
				const Position o = attacker->getPosition();
				from[0] = o.x * 16.0 + 8.0; from[1] = o.y * 16.0 + 8.0; from[2] = o.z * 24.0 + 12.0;
			}
		}
		_view->showHurt(shot || attacker ? from : nullptr, _hurtAmount, attacker || _hurtAmount > 0.55f);
		if (attacker)
		{
			_view->addShake(2.5f);
			_view->rotate((now / 97) % 2 ? 5.0 : -5.0, 3.0); // knocked aside
		}
	}
	_lastHealth = health;
	// badly hurt: the edges of the view pulse red like a heartbeat
	const int maxHealth = std::max(1, (int)unit->getBaseStats()->health);
	const float danger = std::clamp((0.5f - (float)health / maxHealth) / 0.5f, 0.0f, 1.0f);
	const float beat = std::pow(std::max(0.0f, std::sin(now / 1000.0f * 7.5f)), 4.0f);
	_view->setVignette(danger > 0.0f ? danger * (0.45f + 0.55f * beat) : 0.0f);
	// and you hear your heart, in time with the pulse
	const int heartbeat = (int)(now / 1000.0f * 7.5f / (2.0f * 3.14159265f));
	if (danger > 0.0f && heartbeat != _heartbeat && _heartbeat >= 0)
	{
		_sounds->play(FirstPersonSounds::HEARTBEAT, 0, 0.35f + 0.65f * danger);
	}
	_heartbeat = heartbeat;
	const Uint32 age = now - _hurtStart;
	if (!_afterFall) // the next soldier's view still coming in (see deathCam)
	{
		_view->setTint(_hurtStart && age < 450 ? _hurtAmount * (1.0f - age / 450.0f) : 0.0f);
	}
}

/**
 * The soldier we were looking through has gone down, or was taken over: for a moment the view
 * stays with them. Killed: they fall, the view washes red and the helmet camera dies in static.
 * Unconscious: a slow sag into dark blue. Mind controlled: the view drifts, washing orange.
 * Then on to the next soldier, whose view comes in out of it. (With nobody left the game ends
 * the battle, as on the map.)
 * @return True while that's showing.
 */
bool FirstPersonMode::deathCam(Uint32 now, double dt)
{
	BattleUnit *unit = _trackedUnit;
	const bool out = unit && unit->isOut();
	const bool turned = unit && !out && unit->getOriginalFaction() == FACTION_PLAYER && unit->getFaction() != FACTION_PLAYER;
	if (!_deathUnit && (out || turned) && isActive())
	{
		_deathUnit = unit;
		_deathStart = now;
		_fallKind = turned ? FALL_CONTROLLED : unit->getStatus() == STATUS_DEAD ? FALL_KILLED : FALL_UNCONSCIOUS;
		_stepPending = false;
		_throughDoor = false;
		_afterFall = 0;
		_txtMode->setVisible(false);
		_txtShot->setVisible(false);
		_crosshair->setVisible(false);
		_viewModel->setVisible(false); // the weapon falls from the hand
		// what happened, in its colour, and whose camera this was
		const SDL_Color *pal = _state->getPalette();
		const char *title = _fallKind == FALL_KILLED ? "STR_FPS_KILLED" : _fallKind == FALL_UNCONSCIOUS ? "STR_FPS_UNCONSCIOUS" : "STR_FPS_MIND_CONTROLLED";
		// (texts take a colour group's start: their glyph shades run down the group)
		const Uint8 core = _fallKind == FALL_KILLED ? textColor(pal, 255, 70, 50) : _fallKind == FALL_UNCONSCIOUS ? textColor(pal, 140, 180, 255) : textColor(pal, 255, 170, 60);
		const Uint8 halo = _fallKind == FALL_KILLED ? textColor(pal, 150, 10, 10) : _fallKind == FALL_UNCONSCIOUS ? textColor(pal, 30, 40, 140) : textColor(pal, 160, 70, 0);
		_txtBanner->setText(_state->tr(title));
		_txtBanner->setColor(core);
		for (Text *glow : _bannerGlow)
		{
			glow->setText(_state->tr(title));
			glow->setColor(halo);
			glow->setVisible(true);
		}
		_txtBanner->setVisible(true);
		_shadows.front()->setColor(halo); // the banner's shadow is part of the halo
		bool anyoneLeft = false;
		for (const BattleUnit *other : *_save->getUnits())
		{
			if (other != unit && other->getFaction() == FACTION_PLAYER && !other->isOut()) anyoneLeft = true;
		}
		_allLost = !anyoneLeft;
		_txtFallName->setText(anyoneLeft ? unit->getName(_game->getLanguage()) : std::string(_state->tr("STR_FPS_ALL_LOST")));
		_txtFallName->setColor(core);
		_txtFallName->setVisible(true);
		syncShadows();
	}
	if (!_deathUnit)
	{
		// the next soldier's view comes in out of the static or the colour
		if (_afterFall)
		{
			const float p = (now - _afterFall) / (float)AfterFallTime;
			if (p >= 1.0f)
			{
				_afterFall = 0;
				_view->setStatic(0.0f);
				_view->setTint(0.0f);
			}
			else if (_afterFallKind == FALL_KILLED)
			{
				_view->setStatic(1.0f - p);
			}
			else
			{
				_view->setTint(1.0f - p, _afterFallKind == FALL_UNCONSCIOUS ? 1 : 2);
			}
		}
		return false;
	}
	const Uint32 length = _fallKind == FALL_KILLED ? DeathCamTime : _fallKind == FALL_UNCONSCIOUS ? FaintTime : ControlledTime;
	const Uint32 age = now - _deathStart;
	if (age >= length)
	{
		_afterFall = now;
		_afterFallKind = _fallKind;
		_deathUnit = nullptr;
		_trackedUnit = nullptr; // re-place on whoever is next
		_view->setVignette(0.0f);
		_txtBanner->setVisible(false);
		for (Text *glow : _bannerGlow) glow->setVisible(false);
		_txtFallName->setVisible(false);
		syncShadows();
		if (_allLost && _save->getSide() == FACTION_PLAYER && !_state->_battleGame->isBusy())
		{
			// nobody left to play: the only thing left is the end of the turn, where the game ends the battle
			_state->_battleGame->requestEndTurn(false);
		}
		return false;
	}
	const float p = age / (float)length;
	const double floor = _deathUnit->getPosition().z * 24.0 + 5.0;
	switch (_fallKind)
	{
	case FALL_KILLED:
		// down hard, keeling over, red; then the camera's signal goes
		_eyeZ += (floor - _eyeZ) * std::min(1.0, dt * 5.0);
		_view->rotate(dt * 25.0, dt * 30.0 * std::max(0.0f, 1.0f - 2.0f * p));
		_view->setTint(std::min(1.0f, 0.45f + p * 1.6f));
		_view->setStatic(std::clamp((p - 0.45f) / 0.4f, 0.0f, 1.0f));
		if (p > 0.5f) _txtFallName->setText(std::string(_deathUnit->getName(_game->getLanguage())) + " - " + std::string(_state->tr("STR_FPS_SIGNAL_LOST")));
		break;
	case FALL_UNCONSCIOUS:
		// a slow sag, the head dropping, into dark blue
		_eyeZ += (floor - _eyeZ) * std::min(1.0, dt * 1.6);
		_view->rotate(dt * 8.0, -dt * 14.0 * (1.0 - p));
		_view->setTint(std::min(1.0f, 0.25f + p * 1.2f), 1);
		break;
	case FALL_CONTROLLED:
		// the soldier's own mind slips: the view drifts and sways, washing orange
		_view->rotate(std::sin(age * 0.004) * dt * 40.0, std::cos(age * 0.003) * dt * 15.0);
		_view->setTint(std::min(1.0f, 0.2f + p * 1.1f + 0.08f * (float)std::sin(age * 0.02)), 2);
		break;
	}
	_view->setEye(_posX, _posY, _eyeZ);
	_view->invalidate();
	return true;
}

/**
 * A new enemy came into view: stop walking (until the movement keys are let go, so holding
 * a key doesn't carry the soldier on into fire) and say so.
 */
void FirstPersonMode::checkSpotted(BattleUnit *unit, bool playerTurn)
{
	const size_t spotted = unit->getUnitsSpottedThisTurn().size();
	if (unit != _spotter || !playerTurn)
	{
		_spotter = playerTurn ? unit : nullptr;
		_spottedCount = spotted;
		_movementHeld = false;
		return;
	}
	if (spotted > _spottedCount)
	{
		_movementHeld = true;
		_spottedUntil = SDL_GetTicks() + 1500;
	}
	_spottedCount = spotted;
}

// ---------------------------------------------------------------------------------------
// squad bar
// ---------------------------------------------------------------------------------------

/**
 * Works out which unit each squad button stands for: the player's own units in a stable
 * order (so numbers don't shift when someone falls), then any units under mind control.
 */
void FirstPersonMode::refreshSquad()
{
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
	while (_btnSquad.size() < squad.size())
	{
		TextButton *btn = makeButton(SquadX, 16, std::to_string(_btnSquad.size() + 1));
		btn->setGroup(&_squadGroup);
		btn->setVisible(false);
		_squadColor = btn->getColor();
		_btnSquad.push_back(btn);
		Surface *icon = new Surface(7, 8, 0, BarY + 2);
		_state->add(icon);
		icon->setVisible(false);
		_own.insert(icon);
		_squadIcons.push_back(icon);
		_squadStates.push_back(SQUAD_NONE);
	}
	squad.resize(_btnSquad.size(), nullptr);
	bool changed = squad != _squad;
	_squad = squad;
	// how each one is: a fallen or turned soldier's button shows an icon instead of the number
	const SDL_Color *pal = _state->getPalette();
	auto rowNear = [pal](int r, int g, int b) {
		int best = 0, bestDist = INT_MAX;
		for (int row = 0; row < 16; ++row)
		{
			const SDL_Color &c = pal[row * 16 + 3];
			const int dist = (c.r - r) * (c.r - r) * 3 + (c.g - g) * (c.g - g) * 6 + (c.b - b) * (c.b - b);
			if (dist < bestDist) { bestDist = dist; best = row; }
		}
		return (Uint8)(best * 16);
	};
	for (size_t i = 0; i < _squad.size(); ++i)
	{
		const SquadState state = squadState(_squad[i]);
		if (state == _squadStates[i]) continue;
		_squadStates[i] = state;
		changed = true;
		TextButton *btn = _btnSquad[i];
		btn->setText(state == SQUAD_READY ? std::to_string(i + 1) : std::string());
		btn->setColor(state == SQUAD_CONTROLLED ? rowNear(230, 130, 30) : state == SQUAD_DEAD ? rowNear(120, 120, 120)
			: state == SQUAD_UNCONSCIOUS ? rowNear(90, 120, 170) : _squadColor);
		drawSquadIcon(_squadIcons[i], state);
	}
	if (changed)
	{
		layoutTopBar();
	}
}

/**
 * Lays out the squad buttons that are in use, numbered by their slot.
 */
void FirstPersonMode::layoutTopBar()
{
	// the slots with a button: every soldier we came with (the fallen too), and aliens under our control
	_squadSlots.clear();
	for (size_t i = 0; i < _btnSquad.size(); ++i)
	{
		const SquadState state = i < _squadStates.size() ? _squadStates[i] : SQUAD_NONE;
		_btnSquad[i]->setVisible(false);
		_squadIcons[i]->setVisible(false);
		if (state != SQUAD_NONE) _squadSlots.push_back(i);
	}
	const int wanted = std::min<int>(SquadWindow, (int)_squadSlots.size());
	// the suit's buttons, right to left from END, shortened when the bar is narrow
	TextButton *suit[4] = { _btnFly, _btnLight, _btnNight, _btnSpecial };
	const int left = SquadX + PrevNextW + 1;
	int right = 0;
	for (int pass = 0; pass < 2; ++pass)
	{
		right = canvasWidth() - 1 - EndTurnW - 2;
		for (int k = 0; k < 4; ++k)
		{
			if (!suit[k]->getVisible()) continue;
			const std::string &label = pass == 0 ? _suitLong[k] : _suitShort[k];
			suit[k]->setText(label);
			suit[k]->setWidth(labelWidth(label));
			right -= suit[k]->getWidth();
			suit[k]->setX(right);
			right -= 1;
		}
		if (right - 1 - PrevNextW - left >= wanted * (SquadMinW + 1)) break;
	}
	// up to ten squad buttons between previous and next; the window scrolls
	const int available = right - 1 - PrevNextW - left;
	_squadWindow = std::clamp(available / (SquadMinW + 1), 1, std::max(1, wanted));
	_squadScroll = std::clamp(_squadScroll, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	const int width = std::clamp(available / _squadWindow - 1, SquadMinW, 18);
	_btnPrev->setX(SquadX);
	int x = left;
	for (int k = _squadScroll; k < std::min((int)_squadSlots.size(), _squadScroll + _squadWindow); ++k)
	{
		const size_t i = _squadSlots[k];
		_btnSquad[i]->setX(x);
		_btnSquad[i]->setWidth(width);
		_btnSquad[i]->setVisible(true);
		_squadIcons[i]->setX(x + (width - 7) / 2);
		_squadIcons[i]->setVisible(_squadStates[i] != SQUAD_READY);
		x += width + 1;
	}
	_btnNext->setX(x);
}

/**
 * Scrolls the squad buttons by some slots (the mouse wheel over the bar).
 */
void FirstPersonMode::scrollSquad(int step)
{
	const int scroll = std::clamp(_squadScroll + step, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	if (scroll != _squadScroll)
	{
		_squadScroll = scroll;
		layoutTopBar();
	}
}

/**
 * How a squad member is: ready, dead, unconscious, or turned against us by the aliens' mind
 * control (an alien we control is ready while it lasts, and has no button otherwise).
 */
FirstPersonMode::SquadState FirstPersonMode::squadState(const BattleUnit *unit) const
{
	if (!unit)
	{
		return SQUAD_NONE;
	}
	if (unit->getOriginalFaction() != FACTION_PLAYER)
	{
		return unit->getFaction() == FACTION_PLAYER && !unit->isOut() ? SQUAD_READY : SQUAD_NONE;
	}
	if (unit->getStatus() == STATUS_DEAD || (unit->isOut() && unit->getHealth() <= 0))
	{
		return SQUAD_DEAD;
	}
	if (unit->isOut())
	{
		return SQUAD_UNCONSCIOUS;
	}
	return unit->getFaction() == FACTION_PLAYER ? SQUAD_READY : SQUAD_CONTROLLED;
}

/**
 * Pixel-art icons for the squad buttons: a skull for the dead, a caduceus for the unconscious,
 * an alien face for a soldier under the aliens' mind control.
 */
void FirstPersonMode::drawSquadIcon(Surface *icon, SquadState state)
{
	static const char *skull[8] = { ".#####.", "#######", "#..#..#", "#..#..#", "###.###", ".#####.", ".#.#.#.", "..###.." };
	static const char *caduceus[8] = { "##.#.##", ".#####.", "...#...", ".##o##.", "o..#..o", ".oo#oo.", "...#...", "..###.." };
	static const char *alien[8] = { "..###..", ".#####.", "#######", "#oo#oo#", "#.o#o.#", ".#####.", "..###..", "...#..." };
	const SDL_Color *pal = _state->getPalette();
	const char **art = state == SQUAD_DEAD ? skull : state == SQUAD_UNCONSCIOUS ? caduceus : state == SQUAD_CONTROLLED ? alien : nullptr;
	Uint8 ink = 0, ink2 = 0;
	switch (state)
	{
	case SQUAD_DEAD: ink = nearestColor(pal, 235, 230, 205); break;                                    // bone
	case SQUAD_UNCONSCIOUS: ink = nearestColor(pal, 245, 205, 70); ink2 = nearestColor(pal, 120, 230, 140); break; // gold staff, green snakes
	case SQUAD_CONTROLLED: ink = nearestColor(pal, 180, 200, 175); ink2 = nearestColor(pal, 10, 10, 10); break;   // grey skin, black eyes
	default: break;
	}
	icon->clear();
	if (!art)
	{
		return;
	}
	icon->lock();
	for (int y = 0; y < 8; ++y)
	{
		for (int x = 0; x < 7; ++x)
		{
			if (art[y][x] == '#') icon->setPixel(x, y, ink);
			else if (art[y][x] == 'o') icon->setPixel(x, y, ink2);
		}
	}
	icon->unlock();
}

/**
 * Keeps the top bar in sync: button labels, which soldier is selected, and the HUD button
 * only while in 3D.
 */
void FirstPersonMode::updateTopBar(BattleUnit *selected)
{
	refreshSquad();
	// the window of squad buttons follows the selected soldier
	if (selected != _scrolledTo)
	{
		_scrolledTo = selected;
		for (int k = 0; k < (int)_squadSlots.size(); ++k)
		{
			if (_squad[_squadSlots[k]] != selected) continue;
			if (k < _squadScroll) scrollSquad(k - _squadScroll);
			else if (k >= _squadScroll + _squadWindow) scrollSquad(k - _squadScroll - _squadWindow + 1);
		}
	}

	TextButton *squadGroup = nullptr;
	for (size_t i = 0; i < _btnSquad.size(); ++i)
	{
		if (_squad[i] && _squad[i] == selected)
		{
			squadGroup = _btnSquad[i];
		}
	}
	if (squadGroup != _squadGroup)
	{
		_squadGroup = squadGroup;
		for (auto *btn : _btnSquad)
		{
			btn->invalidate();
		}
	}

	const bool active = isActive();
	TextButton *hudGroup = _classicHud ? _btnHud : nullptr;
	if (hudGroup != _hudGroup)
	{
		_hudGroup = hudGroup;
		_btnHud->invalidate();
	}
	_btnHud->setVisible(active);
	_btnEndTurn->setVisible(active); // the classic HUD has its own
	_btnView->setText(_state->tr(active ? "STR_FPS_BUTTON_ISO" : "STR_FPS_BUTTON_3D"));
}

/**
 * Which of our buttons is under this point?
 */
TextButton *FirstPersonMode::buttonAt(double x, double y) const
{
	auto inside = [&](const TextButton *b) {
		return b->getVisible() && x >= b->getX() && x < b->getX() + b->getWidth() && y >= b->getY() && y < b->getY() + b->getHeight();
	};
	for (TextButton *b : { _btnView, _btnHud, _btnInventory, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnPrev, _btnNext })
	{
		if (inside(b)) return b;
	}
	for (TextButton *b : _btnSquad)
	{
		if (inside(b)) return b;
	}
	return nullptr;
}

/**
 * Acts on a click on one of our buttons.
 */
void FirstPersonMode::buttonClicked(TextButton *button, Action *action)
{
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
		toggleNightVision(_save->getSelectedUnit(), action);
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
		for (size_t i = 0; i < _btnSquad.size(); ++i)
		{
			if (button == _btnSquad[i] && _squad[i])
			{
				selectUnit(_squad[i]);
			}
		}
	}
}

/**
 * Selects a soldier from the squad bar, the same way the next-soldier button does.
 */
void FirstPersonMode::selectUnit(BattleUnit *unit)
{
	if (!_state->allowButtons() || unit->isOut() || unit->getFaction() != FACTION_PLAYER)
	{
		return;
	}
	_save->setSelectedUnit(unit);
	_state->updateSoldierInfo();
	_state->_map->getCamera()->centerOnPosition(unit->getPosition());
	_state->_battleGame->cancelAllActions();
	_state->_battleGame->getCurrentAction()->actor = unit;
	_state->_battleGame->setupCursor();
}

// ---------------------------------------------------------------------------------------
// per frame, events, blitting
// ---------------------------------------------------------------------------------------

/**
 * Per-frame update. On your turn the camera is the selected soldier, who moves, turns and
 * fires; on the aliens' turn it stays with your last soldier (look around only) and shows
 * whatever that soldier's squad can see.
 */
void FirstPersonMode::think()
{
	const Uint32 now = SDL_GetTicks();
	lastAlive = now;
	const double dt = _lastTick ? std::min<Uint32>(100, now - _lastTick) / 1000.0 : 0.0;
	// another screen was on top (popup, next turn, inventory): it had the mouse released
	const bool resumed = _lastTick && now - _lastTick > 250;
	_lastTick = now;
	if (wantsMouseCapture() != _captured || resumed)
	{
		_captured = wantsMouseCapture();
		_ignoreMotionUntil = now + 150;
	}

	if (deathCam(now, dt))
	{
		return;
	}
	const bool playerTurn = _save->getSide() == FACTION_PLAYER;
	BattleUnit *selected = _save->getSelectedUnit();
	// the soldier we were with is gone (killed, stunned, mind controlled...): carry on with the next one
	if (_wanted && playerTurn && (!selected || selected->isOut() || selected->getFaction() != FACTION_PLAYER)
		&& !_state->_battleGame->isBusy() && _state->_popups.empty())
	{
		_state->selectNextPlayerUnit();
		selected = _save->getSelectedUnit();
	}
	if (playerTurn && selected && selected->getFaction() == FACTION_PLAYER && !selected->isOut())
	{
		_lastPlayerUnit = selected;
	}
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
	BattleUnit *unit = playerTurn ? selected : (Options::fpsAlienTurn3D ? _lastPlayerUnit : nullptr);
	if (unit && unit->isOut())
	{
		unit = nullptr;
	}
	const bool show = _wanted && unit;
	if (show != isActive())
	{
		setActive(show);
	}
	updateTopBar(selected);
	updateCard();
	updateMusic(now);
	updateMapAim();
	updateFlight(show ? unit : nullptr);
	if (!show)
	{
		updatePanel(nullptr);
		updateArmor(nullptr);
		updateLift(nullptr, now);
		updateCrosshair(nullptr, playerTurn);
		updateBanner(playerTurn);
		updateViewModel(nullptr);
		syncShadows();
		return;
	}
	if (playerTurn && Options::fpsDoomMode && unit->getFaction() == FACTION_PLAYER)
	{
		applyDoom(unit);
	}
	turnWithKeys(dt);
	_view->setFieldOfView(Options::fpsFieldOfView);
	_view->setSmoothLighting(Options::fpsSmoothLighting);
	checkSpotted(unit, playerTurn);
	checkHurt(unit, now);
	checkNearMiss();
	updateRecoil(unit, dt);
	// FPS-style auto: holding the trigger fires the next burst as soon as the last is done
	if (_triggerHeld && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)))
	{
		_triggerHeld = false; // released while another screen had the mouse
	}
	if (_triggerHeld && playerTurn && _mode == BA_AUTOSHOT && freeAimOn(_mode) && _mouseLook && canCommit()
		&& !_state->_battleGame->isBusy() && unit->getFaction() == FACTION_PLAYER)
	{
		BattleItem *weapon = activeWeapon(unit);
		BattleActionCost cost(BA_AUTOSHOT, unit, weapon);
		if (weapon && weapon->getAmmoForAction(BA_AUTOSHOT) && (Options::fpsDoomMode || cost.haveTU()))
		{
			fire(_view->getWidth() / 2, _view->getHeight() / 2);
		}
		else
		{
			_triggerHeld = false; // empty, or out of time
		}
	}
	_view->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
	// in the dark the soldier's personal light becomes a shoulder flashlight
	_view->setFlashlight(Options::fpsFlashlight && _save->getTogglePersonalLight() && _save->getGlobalShade() >= 6);
	updateCamera(unit, playerTurn, dt);
	if (playerTurn)
	{
		faceLookDirection(unit);
		updateFireMode(unit);
		aim(unit);
	}
	std::vector<std::pair<const BattleUnit*, int>> markers;
	for (int i = 0; i < BattlescapeState::VISIBLE_MAX; ++i)
	{
		BattleUnit *enemy = _state->_visibleUnit[i];
		if (enemy && !enemy->isOut() && _state->_btnVisibleUnit[i]->getVisible())
		{
			markers.push_back({ enemy, i + 1 });
		}
	}
	_view->setMarkers(markers);
	updatePanel(unit);
	updateArmor(unit);
	updateLift(unit, now);
	updateCrosshair(unit, playerTurn);
	updateBanner(playerTurn);
	updateViewModel(unit);
	syncShadows();
	_view->invalidate();
}

/**
 * Gets the first look at every event.
 * Keys: view toggle anywhere; in 3D the mouse mode, classic HUD and doom toggles, fire mode
 * and hand keys, up/down, the enemy keys turn the view to that enemy, and the movement keys
 * (polled each frame, swallowed here so they don't fire battlescape hotkeys); everything else
 * goes on to the normal hotkeys.
 * Mouse: in mouse look it turns the view, left button fires, right button cancels a picked
 * fire mode, the wheel changes fire mode; with a free cursor our buttons get it first, then
 * the classic HUD (if shown), and a left click on the view fires at the cursor.
 * @return True if the event was consumed.
 */
bool FirstPersonMode::handle(Action *action)
{
	const SDL_Event &ev = *action->getDetails();
	const bool active = isActive();

	// which squad button the free cursor is over, for the soldier card (never consumed)
	if (ev.type == SDL_MOUSEMOTION && !wantsMouseCapture())
	{
		action->setMouseAction(ev.motion.x, ev.motion.y, 0, 0);
		const TextButton *hover = buttonAt(action->getAbsoluteXMouse() / _ui, action->getAbsoluteYMouse() / _ui);
		_liftHover = liftAt(action->getAbsoluteXMouse() / _ui, action->getAbsoluteYMouse() / _ui);
		_hoverIndex = -1;
		for (size_t i = 0; i < _btnSquad.size(); ++i)
		{
			if (hover == _btnSquad[i]) _hoverIndex = (int)i;
		}
	}

	if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
	{
		if (_game->isCtrlPressed() || _game->isAltPressed())
		{
			return false;
		}
		const SDLKey key = ev.key.keysym.sym;
		const bool down = ev.type == SDL_KEYDOWN;
		if (key == Options::keyBattleFirstPerson)
		{
			if (down) toggleView();
			return true;
		}
		if (!active)
		{
			return false;
		}
		if (key == Options::keyFpsMouseMode) { if (down) setMouseLook(!_mouseLook); return true; }
		if (key == Options::keyFpsClassicHud) { if (down) setClassicHud(!_classicHud); return true; }
		if (key == Options::keyFpsDoomMode) { if (down) Options::fpsDoomMode = !Options::fpsDoomMode; return true; }
		if (key == Options::keyFpsNextMode) { if (down) cycleMode(1); return true; }
		if (key == Options::keyFpsModeSnap) { if (down) selectMode(BA_SNAPSHOT); return true; }
		if (key == Options::keyFpsModeAimed) { if (down) selectMode(BA_AIMEDSHOT); return true; }
		if (key == Options::keyFpsModeAuto) { if (down) selectMode(BA_AUTOSHOT); return true; }
		if (key == Options::keyFpsSwitchHand) { if (down) switchHand(); return true; }
		if (key == Options::keyFpsOpenDoor) { if (down) openDoor(_save->getSelectedUnit()); return true; }
		if (key == Options::keyFpsFlare && Options::fpsFlares) { if (down) throwFlare(); return true; }
		if (key == Options::keyFpsUp) { if (down) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_UP, action); return true; }
		if (key == Options::keyFpsDown) { if (down) moveUpDown(_save->getSelectedUnit(), Pathfinding::DIR_DOWN, action); return true; }
		if (key == Options::keyFpsFlight) { if (down) toggleFlight(_save->getSelectedUnit()); return true; }
		if (key == Options::keyFpsJump) { if (down) jump(_save->getSelectedUnit()); return true; }
		if (key == Options::keyFpsNightVision) { if (down) toggleNightVision(_save->getSelectedUnit(), action); return true; }
		const SDLKey enemyKeys[] = { Options::keyBattleCenterEnemy1, Options::keyBattleCenterEnemy2, Options::keyBattleCenterEnemy3,
			Options::keyBattleCenterEnemy4, Options::keyBattleCenterEnemy5, Options::keyBattleCenterEnemy6, Options::keyBattleCenterEnemy7,
			Options::keyBattleCenterEnemy8, Options::keyBattleCenterEnemy9, Options::keyBattleCenterEnemy10 };
		for (int i = 0; i < 10; ++i)
		{
			if (key == enemyKeys[i] && key != SDLK_UNKNOWN)
			{
				if (down) lookAtEnemy(i);
				return true;
			}
		}
		return key == Options::keyFpsForward || key == Options::keyFpsBack
			|| key == Options::keyFpsStrafeLeft || key == Options::keyFpsStrafeRight;
	}

	if (ev.type != SDL_MOUSEMOTION && ev.type != SDL_MOUSEBUTTONDOWN && ev.type != SDL_MOUSEBUTTONUP)
	{
		return false;
	}
	const bool wheel = ev.type == SDL_MOUSEBUTTONDOWN && (ev.button.button == SDL_BUTTON_WHEELUP || ev.button.button == SDL_BUTTON_WHEELDOWN);

	if (active && wantsMouseCapture())
	{
		if (ev.type == SDL_MOUSEMOTION)
		{
			if (SDL_GetTicks() >= _ignoreMotionUntil)
			{
				look(ev.motion.xrel, ev.motion.yrel);
			}
		}
		else if (wheel)
		{
			cycleMode(ev.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
		}
		else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_MIDDLE && !_dragLook)
		{
			setMouseLook(false);
		}
		else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT && !_dragLook)
		{
			_triggerHeld = true;
			fire(_view->getWidth() / 2, _view->getHeight() / 2);
		}
		else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
		{
			_triggerHeld = false;
		}
		else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT && !_dragLook)
		{
			_state->_battleGame->cancelCurrentAction();
		}
		else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_RIGHT && _dragLook)
		{
			_dragLook = false;
		}
		return true;
	}

	if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP)
	{
		action->setMouseAction(ev.button.x, ev.button.y, 0, 0);
		const double mx = action->getAbsoluteXMouse(), my = action->getAbsoluteYMouse();
		TextButton *hit = buttonAt(mx / _ui, my / _ui); // our buttons are on the canvas
		if (ev.type == SDL_MOUSEBUTTONUP)
		{
			// let every button see the release so none stays drawn pressed
			for (TextButton *b : _btnSquad) handleOnCanvas(b, action);
			for (TextButton *b : { _btnView, _btnHud, _btnInventory, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnPrev, _btnNext }) handleOnCanvas(b, action);
		}
		const bool overSquad = hit && (hit == _btnPrev || hit == _btnNext || std::find(_btnSquad.begin(), _btnSquad.end(), hit) != _btnSquad.end());
		if (wheel && overSquad)
		{
			scrollSquad(ev.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
			return true;
		}
		if (hit)
		{
			if (ev.type == SDL_MOUSEBUTTONDOWN)
			{
				handleOnCanvas(hit, action); // pressed look and click sound
				if (ev.button.button == SDL_BUTTON_LEFT)
				{
					buttonClicked(hit, action);
				}
			}
			return true;
		}
		if (!active)
		{
			return false;
		}
		// the lift arrows ride the lift (the up/down keys do the same)
		if (const int lift = liftAt(mx / _ui, my / _ui))
		{
			if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
			{
				moveUpDown(_save->getSelectedUnit(), lift > 0 ? Pathfinding::DIR_UP : Pathfinding::DIR_DOWN, action);
			}
			return true;
		}
		// the visible enemy buttons turn the view to that enemy (the classic HUD's own handling centres the map)
		if (!_classicHud && ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
		{
			for (int i = 0; i < BattlescapeState::VISIBLE_MAX; ++i)
			{
				const InteractiveSurface *b = _state->_btnVisibleUnit[i];
				if (b->getVisible() && mx >= b->getX() && mx < b->getX() + b->getWidth() && my >= b->getY() && my < b->getY() + b->getHeight())
				{
					lookAtEnemy(i);
					return true;
				}
			}
		}
		const bool overClassicHud = _classicHud && my >= _state->_icons->getY();
		if (ev.type == SDL_MOUSEBUTTONDOWN && !overClassicHud)
		{
			if (wheel)
			{
				cycleMode(ev.button.button == SDL_BUTTON_WHEELUP ? -1 : 1);
				return true;
			}
			if (ev.button.button == SDL_BUTTON_MIDDLE)
			{
				setMouseLook(true);
				return true;
			}
			if (ev.button.button == SDL_BUTTON_RIGHT)
			{
				_dragLook = true;
				return true;
			}
			if (ev.button.button == SDL_BUTTON_LEFT)
			{
				fire((int)(mx / _ui), (int)(my / _ui)); // the view is on the canvas
				return true;
			}
		}
	}
	if (!active)
	{
		return false;
	}
	// with the classic HUD up, the normal handling takes it from here; without it there's nothing else to click
	return !_classicHud;
}

/**
 * Blits the 3D screen without the classic HUD: our surfaces, warnings and debug text only.
 * @return False when the state should blit everything as usual.
 */
bool FirstPersonMode::blit()
{
	SDL_Surface *screen = _game->getScreen()->getSurface();
	const bool full3D = isActive() && !_classicHud;
	// the 3D view at the back, magnified on big screens
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
	// the game's own surfaces (in full 3D only its warnings and debug text)
	for (Surface *surface : _state->_surfaces)
	{
		if (!_own.count(surface) && (!full3D || surface == _state->_warning || surface == _state->_txtDebug))
		{
			surface->blit(screen);
		}
	}
	// our overlays: straight onto the screen, or onto the canvas and magnified
	if (!_canvas)
	{
		for (Surface *surface : _state->_surfaces)
		{
			if (surface != _view && _own.count(surface)) surface->blit(screen);
		}
	}
	else
	{
		_canvas->clear();
		for (Surface *surface : _state->_surfaces)
		{
			if (surface != _view && _own.count(surface)) surface->blit(_canvas->getSurface());
		}
		magnify(screen, true);
	}
	// the original's visible enemy buttons (bottom right; clicking one turns the view to it)
	if (full3D)
	{
		for (int i = 0; i < BattlescapeState::VISIBLE_MAX; ++i)
		{
			if (_state->_btnVisibleUnit[i]->getVisible())
			{
				_state->_btnVisibleUnit[i]->blit(screen);
				_state->_numVisibleUnit[i]->blit(screen);
			}
		}
	}
	return true;
}

/**
 * Copies the canvas onto the screen _ui times bigger (skipping its empty pixels if transparent).
 */
void FirstPersonMode::magnify(SDL_Surface *screen, bool transparent)
{
	const SDL_Color *palette = _state->getPalette();
	if (!std::equal(palette, palette + 256, _canvasPalette, [](const SDL_Color &a, const SDL_Color &b) { return a.r == b.r && a.g == b.g && a.b == b.b; }))
	{
		std::copy(palette, palette + 256, _canvasPalette);
		_canvas->setPalette(_canvasPalette);
	}
	SDL_Surface *canvas = _canvas->getSurface();
	const int sw = std::min(screen->w, canvas->w * _ui), sh = std::min(screen->h, canvas->h * _ui);
	SDL_LockSurface(screen);
	SDL_LockSurface(canvas);
	// each canvas row widened once, then laid on its _ui screen rows (copied whole where opaque)
	_magnifyRow.resize(sw);
	for (int cy = 0; cy * _ui < sh; ++cy)
	{
		const Uint8 *src = (const Uint8 *)canvas->pixels + cy * canvas->pitch;
		for (int x = 0, cx = 0; x < sw; ++cx)
		{
			for (int k = 0; k < _ui && x < sw; ++k) _magnifyRow[x++] = src[cx];
		}
		for (int y = cy * _ui; y < std::min(sh, (cy + 1) * _ui); ++y)
		{
			Uint8 *dst = (Uint8 *)screen->pixels + y * screen->pitch;
			if (!transparent)
			{
				std::copy(_magnifyRow.begin(), _magnifyRow.end(), dst);
				continue;
			}
			for (int x = 0; x < sw; ++x)
			{
				if (_magnifyRow[x]) dst[x] = _magnifyRow[x];
			}
		}
	}
	SDL_UnlockSurface(canvas);
	SDL_UnlockSurface(screen);
}

}
