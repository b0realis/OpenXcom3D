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
 * FirstPersonMapState.cpp: the tactical map (see FirstPersonMapState.h).
 */
#include "FirstPersonMapState.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "FirstPersonModeShared.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Pathfinding.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/InteractiveSurface.h"
#include "../Engine/Language.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Timer.h"
#include "../Engine/Unicode.h"
#include "../Interface/Text.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{

namespace
{

/// A tile's little picture is this many pixels square (the game's SCANG.DAT), and its number there is the part's
/// map number and this (see MiniMapView).
const int CellPixels = 4, TilePictures = 35;
/// The map is enlarged by whole steps, 1 to ZoomMost; what the player last chose is kept for the next time, with the
/// names, the cones and the rest on or off (0: not chosen yet, the map picks what suits the screen). The zoom is kept
/// with the player's options too (the unlisted "fpsMapZoom"), so the next battle and the next day's game open the
/// map as it was left (the owner: "minimap should remember your previously set zoom level"). The help is off until
/// it is asked for.
const int ZoomMost = 8;
int Zoom = 0;
bool Names = true, Cones = true, Weapons = true, Reach = true, Helmet = true, Help = false;
/// A cone opens this many degrees either side of the way its unit faces (the game's units see a quarter circle).
const double ConeHalf = 45.0;
/// The pointer is on a dot within this many pixels of its rim; a press that moves further than DragStart is a drag.
const int DotReach = 3, DragStart = 3;
/// The buttons: this high, this far apart and from the screen's edges, their words this far inside; a line of words
/// is this high.
const int ButtonHigh = 13, ButtonGap = 2, ButtonPad = 4, WordsHigh = 9;
/// The helmet camera's picture is this big at the least (times the screen's magnification), in the bottom right corner.
const int HelmetWide = 112, HelmetHigh = 70;
/// The pieces of the line along the bottom (the name, the health...) are this far apart.
const int StatusGap = 8;

}

/**
 * Sets the map up all over the screen: the battle's palette, the game's little pictures of the tiles, the level the
 * battle's view looks at, the selected soldier (or the view's middle) in the middle, and the zoom last chosen (the
 * first time: the biggest at which a 40 tile map still fits the screen, 2 at least on a screen that has room).
 * @param camera The battle's own view (the level looked at; left looking where the map did).
 * @param save The battle.
 * @param maxShade The darkest a seen tile is shown (the game's own map screen's).
 * @param setup What the 3D view hands over (see Setup).
 */
FirstPersonMapState::FirstPersonMapState(Camera *camera, SavedBattleGame *save, int maxShade, const Setup &setup) : _camera(camera), _save(save), _maxShade(maxShade), _setup(setup)
{
	// (all over the screen, or in the window it is given: see Setup::window)
	const int width = embedded() ? setup.window.w : Options::baseXResolution, height = embedded() ? setup.window.h : Options::baseYResolution;
	_ui = std::max(1, setup.uiScale);
	_view = new InteractiveSurface(width, height, embedded() ? setup.window.x : 0, embedded() ? setup.window.y : 0);
	save->setPaletteByDepth(this);
	add(_view);
	_tiles = _game->getMod()->getSurfaceSet("SCANG.DAT");
	_terrain = new Surface(save->getMapSizeX() * CellPixels, save->getMapSizeY() * CellPixels);
	_words = new Text(width, WordsHigh + 2, 0, 0);
	_words->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
	_words->setPalette(getPalette());
	_words->setSmall();

	// The colours, the nearest the battle's palette has (a mod's palette is its own). A word's colour is the bright
	// end of a run the letters shade down from: kept off the run's dark end.
	const SDL_Color *palette = getPalette();
	auto bright = [&](int red, int green, int blue) {
		const Uint8 colour = nearestColor(palette, red, green, blue);
		return (Uint8)((colour & 0xF0) | std::min(colour & 0x0F, 9));
	};
	_black = nearestColor(palette, 0, 0, 0);
	_dark = nearestColor(palette, 24, 28, 40);
	_panel = nearestColor(palette, 20, 24, 36);
	_edge = nearestColor(palette, 110, 130, 170);
	_grey = bright(120, 120, 130);
	_white = bright(240, 240, 240);
	_ours = bright(60, 220, 70);
	_selected = bright(70, 140, 255);
	_enemy = bright(240, 50, 50);
	_other = bright(240, 200, 60);
	_item = bright(200, 200, 200);
	// (the bottom line's: the 3D view's own panel's where it handed them over, else the nearest the map has)
	_nameColour = setup.hasPanelColours ? setup.nameColour : _selected;
	_healthColour = setup.hasPanelColours ? setup.healthColour : _enemy;
	_timeColour = setup.hasPanelColours ? setup.timeColour : _ours;
	_weaponColour = setup.hasPanelColours ? setup.weaponColour : _other;

	_level = std::clamp(camera->getViewLevel(), 0, save->getMapSizeZ() - 1);
	if (Zoom <= 0)
	{
		Zoom = Options::fpsMapZoom > 0 ? Options::fpsMapZoom : std::min(Options::baseXResolution, Options::baseYResolution) / (40 * CellPixels);
	}
	Zoom = std::clamp(Zoom, 1, ZoomMost * _ui);
	// (in a window: its own, at first the biggest at which a 40 tile map fits it)
	_zoom = embedded() ? std::clamp(std::min(width, height) / (40 * CellPixels), 1, ZoomMost * _ui) : Zoom;
	const BattleUnit *selected = save->getSelectedUnit();
	if (selected && !selected->isOut())
	{
		centreOn(selected);
	}
	else
	{
		_centreX = camera->getCenterPosition().x + 0.5;
		_centreY = camera->getCenterPosition().y + 0.5;
	}
	layOut();
	// The helmet camera (the 3D view's own kind, made by it): in the bottom right corner, over the line of words (and
	// over the help's rows while they show, see placeHelmet).
	if (_setup.makeCamera && _setup.aimCamera)
	{
		// (three tenths of the screen's width where that is more: a bigger screen has room for a bigger picture)
		const int wide = std::max(big(HelmetWide), width * 3 / 10), high = wide * HelmetHigh / HelmetWide;
		_helmet = _setup.makeCamera(this, wide, high);
		if (_helmet)
		{
			_helmet->setX(width - wide - big(ButtonGap) - _ui);
			_helmet->setVisible(false);
			placeHelmet();
		}
	}
	updateReach();
	updateHelmet();

	_view->onMousePress((ActionHandler)&FirstPersonMapState::viewPress);
	_view->onMouseRelease((ActionHandler)&FirstPersonMapState::viewRelease);
	_view->onMouseOver((ActionHandler)&FirstPersonMapState::viewOver);
	_clock = new Timer(125);
	_clock->onTimer((StateHandler)&FirstPersonMapState::animate);
	_clock->start();
	draw();
}

/**
 * Deletes what the map made for itself (the view is the screen's, deleted with it).
 */
FirstPersonMapState::~FirstPersonMapState()
{
	delete _clock;
	delete _terrain;
	delete _words;
}

/**
 * A tile's side on the screen, in pixels.
 * @return The pixels.
 */
int FirstPersonMapState::cell() const
{
	return CellPixels * _zoom;
}

/**
 * Lays the buttons out along the top, each as wide as its words: the level's two, the zoom's two, the map back on
 * the soldier, the next soldier, then what shows or not: the names, the weapons, the cones, the reach, the camera; and
 * OK in the top right corner, the help left of it. On a screen too narrow for one row they go on in a second. And
 * counts the rows the longest help needs on this screen.
 */
void FirstPersonMapState::layOut()
{
	struct Said { ButtonKind kind; const char *words, *tip; };
	static const Said all[] = {
		{ BUTTON_CLOSE, "STR_FPS_TMAP_CLOSE", "STR_FPS_TMAP_CLOSE_TIP" }, { BUTTON_HELP, "STR_FPS_TMAP_HELP", "STR_FPS_TMAP_HELP_TIP" },
		{ BUTTON_LEVEL_UP, "STR_FPS_TMAP_UP", "STR_FPS_TMAP_UP_TIP" }, { BUTTON_LEVEL_DOWN, "STR_FPS_TMAP_DOWN", "STR_FPS_TMAP_DOWN_TIP" },
		{ BUTTON_ZOOM_OUT, "STR_FPS_TMAP_ZOOM_OUT", "STR_FPS_TMAP_ZOOM_OUT_TIP" }, { BUTTON_ZOOM_IN, "STR_FPS_TMAP_ZOOM_IN", "STR_FPS_TMAP_ZOOM_IN_TIP" },
		{ BUTTON_CENTRE, "STR_FPS_TMAP_CENTRE", "STR_FPS_TMAP_CENTRE_TIP" }, { BUTTON_NEXT, "STR_FPS_TMAP_NEXT", "STR_FPS_TMAP_NEXT_TIP" },
		{ BUTTON_NAMES, "STR_FPS_TMAP_NAMES", "STR_FPS_TMAP_NAMES_TIP" }, { BUTTON_WEAPONS, "STR_FPS_TMAP_WEAPONS", "STR_FPS_TMAP_WEAPONS_TIP" },
		{ BUTTON_CONES, "STR_FPS_TMAP_CONES", "STR_FPS_TMAP_CONES_TIP" }, { BUTTON_REACH, "STR_FPS_TMAP_REACH", "STR_FPS_TMAP_REACH_TIP" },
		{ BUTTON_CAMERA, "STR_FPS_TMAP_CAMERA", "STR_FPS_TMAP_CAMERA_TIP" } };
	_buttons.clear();
	int x = big(ButtonGap), y = big(ButtonGap), closeLeft = _view->getWidth();
	for (const Said &said : all)
	{
		// (in a window no OK: it never closes; with no helmet camera, no button for it)
		if ((said.kind == BUTTON_CLOSE && embedded()) || (said.kind == BUTTON_CAMERA && !(_setup.makeCamera && _setup.aimCamera))) continue;
		Button button;
		button.kind = said.kind;
		button.words = tr(said.words);
		// (the level's and the zoom's tips name the wheel as the setting "Tactical map: wheel changes level" has it)
		const bool wheelTip = Options::fpsMapWheelLevels && (said.kind == BUTTON_LEVEL_UP || said.kind == BUTTON_LEVEL_DOWN || said.kind == BUTTON_ZOOM_OUT || said.kind == BUTTON_ZOOM_IN);
		button.tip = tr(wheelTip ? std::string(said.tip) + "_WHEEL" : std::string(said.tip));
		_words->setText(button.words);
		const int wide = std::max(big(ButtonHigh), big(_words->getTextWidth()) + 2 * big(ButtonPad));
		if (said.kind == BUTTON_CLOSE || said.kind == BUTTON_HELP)
		{
			// (the top right corner, filled from the right: OK, then the help)
			closeLeft -= big(ButtonGap) + wide;
			button.box.x = (Sint16)closeLeft;
			button.box.y = (Sint16)big(ButtonGap);
		}
		else
		{
			// (the next row, clear of OK, where this one is full)
			if (x + wide > (y == big(ButtonGap) ? closeLeft - big(ButtonGap) : _view->getWidth() - big(ButtonGap)))
			{
				x = big(ButtonGap);
				y += big(ButtonHigh) + big(ButtonGap);
			}
			button.box.x = (Sint16)x;
			button.box.y = (Sint16)y;
			x += wide + big(ButtonGap);
			// (a little apart after each group: the level's, the zoom's, the soldiers')
			if (said.kind == BUTTON_LEVEL_DOWN || said.kind == BUTTON_ZOOM_IN || said.kind == BUTTON_NEXT) x += 3 * big(ButtonGap);
		}
		button.box.w = (Uint16)wide;
		button.box.h = (Uint16)big(ButtonHigh);
		_buttons.push_back(button);
	}
	_cornerLeft = closeLeft;
	// The help: how the map is worked, or a button's use; the rows the longest of them needs here.
	_hint = tr("STR_FPS_TMAP_HINT");
	const int room = _view->getWidth() - 2 * (big(ButtonGap) + _ui);
	_helpRows = std::max(1, (int)wrapped(_hint, room).size());
	for (const Button &button : _buttons)
	{
		_helpRows = std::max(_helpRows, (int)wrapped(button.tip, room).size());
	}
}

/**
 * How wide words are on the map (see write).
 * @param text The words.
 * @return The pixels.
 */
int FirstPersonMapState::wideOf(const std::string &text) const
{
	_words->setText(text);
	return big(_words->getTextWidth());
}

/**
 * Breaks words into rows no wider than the room there is, at their spaces (a word wider than the room has a row of
 * its own); a row that holds whole sentences (two spaces between them) is broken after the last whole one, so a
 * sentence isn't left with a word of it alone on the next row.
 * @param text The words.
 * @param room The pixels a row may be wide.
 * @return The rows, top to bottom.
 */
std::vector<std::string> FirstPersonMapState::wrapped(const std::string &text, int room) const
{
	std::vector<std::string> rows;
	std::string row;
	auto close = [&]() {
		while (!row.empty() && row.back() == ' ') row.pop_back();
		if (!row.empty()) rows.push_back(row);
		row.clear();
	};
	for (size_t from = 0; from <= text.size();)
	{
		size_t space = text.find(' ', from);
		if (space == std::string::npos) space = text.size();
		const std::string word = text.substr(from, space - from);
		from = space + 1;
		// (nothing between two spaces: the wider gap after a sentence is kept inside a row)
		if (word.empty())
		{
			if (!row.empty()) row += ' ';
			continue;
		}
		const std::string longer = row.empty() ? word : row + " " + word;
		if (!row.empty() && wideOf(longer) > room)
		{
			// (the sentence begun in this row goes down with the word, if a whole one stays; none is begun where the
			// row ends at a sentence's gap)
			const size_t sentence = row.back() == ' ' ? std::string::npos : row.rfind("  ");
			std::string begun;
			if (sentence != std::string::npos && sentence > 0)
			{
				begun = row.substr(sentence + 2);
				row.erase(sentence);
			}
			close();
			row = begun.empty() ? word : begun + " " + word;
		}
		else
		{
			row = longer;
		}
	}
	close();
	return rows;
}

/**
 * Puts the helmet camera's picture over the line along the bottom, and over the help's rows while the help shows.
 */
void FirstPersonMapState::placeHelmet()
{
	if (!_helmet) return;
	const int rows = Help ? _helpRows : 0;
	_helmet->setY(_view->getHeight() - _helmet->getHeight() - big(WordsHigh) - 3 * big(ButtonGap) - _ui - rows * (big(WordsHigh) + _ui));
	_dirty = true;
}

/**
 * Draws the tiles' little pictures for the level looked at, a pixel a pixel: every level from the ground up to it
 * laid over the ones below, as the game's map screen has them (a tile never seen black, one seen as dark as the
 * light on it, no darker than the game's map shows).
 */
void FirstPersonMapState::rebuildTerrain()
{
	_terrain->clear();
	_terrain->lock();
	for (int level = 0; level <= _level; ++level)
	{
		for (int y = 0; y < _save->getMapSizeY(); ++y)
		{
			for (int x = 0; x < _save->getMapSizeX(); ++x)
			{
				Tile *tile = _save->getTile(Position(x, y, level));
				if (!tile) continue;
				for (int part = O_FLOOR; part < O_MAX; ++part)
				{
					const MapData *data = tile->getMapData((TilePart)part);
					if (!data || !data->getMiniMapIndex()) continue;
					Surface *picture = _tiles->getFrame(data->getMiniMapIndex() + TilePictures);
					if (!picture) continue;
					const int shade = tile->isDiscovered(O_FLOOR) ? std::min(tile->getShade(), _maxShade) : 16;
					picture->blitNShade(_terrain, x * CellPixels, y * CellPixels, shade);
				}
			}
		}
	}
	_terrain->unlock();
	_terrainLevel = _level;
}

/**
 * Whether the map shows a unit: any of ours standing; anyone else standing who is seen (or with the game's debug
 * view, or its hunt for the last aliens): what the game's map shows.
 * @param unit The unit.
 * @return True if it has a dot.
 */
bool FirstPersonMapState::shows(const BattleUnit *unit) const
{
	return unit && !unit->isOut() && unit->getTile()
		&& (unit->getFaction() == FACTION_PLAYER || unit->getVisible() || _save->getDebugMode() || _save->getBughuntMode());
}

/**
 * A soldier's number: its place among the squad's buttons in the 3D view.
 * @param unit The unit.
 * @return The number from 1, or 0 for a unit that has no button.
 */
int FirstPersonMapState::numberOf(const BattleUnit *unit) const
{
	const auto found = std::find(_setup.squad.begin(), _setup.squad.end(), unit);
	return found != _setup.squad.end() ? (int)(found - _setup.squad.begin()) + 1 : 0;
}

/**
 * A unit's kind in a few letters: its race's name (its own name, without a race or a name for it), the first three
 * letters and a full stop ("Sec." for a Sectoid); a name of four letters or fewer whole.
 * @param unit The unit.
 * @return The letters.
 */
std::string FirstPersonMapState::kindOf(const BattleUnit *unit) const
{
	std::string name;
	const std::string race = unit->getUnitRules() ? unit->getUnitRules()->getRace() : std::string();
	if (!race.empty())
	{
		name = tr(race);
		if (name == race) name.clear(); // (no name for it in this language)
	}
	if (name.empty()) name = unit->getName(_game->getLanguage());
	const UString letters = Unicode::convUtf8ToUtf32(name);
	if (letters.size() <= 4) return name;
	return Unicode::convUtf32ToUtf8(letters.substr(0, 3)) + ".";
}

/**
 * Writes words on the map, as many times as big as the screen's magnification says (each pixel of the game's small
 * letters a square).
 * @param text The words.
 * @param x, y Their top left corner.
 * @param colour Their colour.
 * @param backed On a dark plate of their own (over the map; on a button or a bar, not).
 * @return How wide they are.
 */
int FirstPersonMapState::write(const std::string &text, int x, int y, Uint8 colour, bool backed)
{
	_words->setText(text);
	_words->setColor(colour);
	const int letters = _words->getTextWidth(), wide = big(letters);
	if (backed) _view->drawRect((Sint16)(x - _ui), (Sint16)(y - _ui), (Sint16)(wide + 2 * _ui), (Sint16)(big(WordsHigh) + _ui), _panel);
	_words->draw();
	if (_ui == 1)
	{
		_words->blitNShade(_view, x, y, 0);
		return wide;
	}
	for (int row = 0; row < _words->getHeight(); ++row)
	{
		for (int column = 0; column <= letters && column < _words->getWidth(); ++column)
		{
			const Uint8 pixel = _words->getPixel(column, row);
			if (pixel) _view->drawRect((Sint16)(x + column * _ui), (Sint16)(y + row * _ui), (Sint16)_ui, (Sint16)_ui, pixel);
		}
	}
	return wide;
}

/**
 * Draws a unit's cone: a quarter circle the way it looks, every other pixel filled (the map shows through), its two
 * sides drawn whole.
 * @param x, y The unit's middle.
 * @param from Where the cone begins, off the middle (the dot's rim).
 * @param degrees The way it looks (0 east, 90 south).
 * @param reach How far the cone reaches.
 * @param colour Its colour.
 */
void FirstPersonMapState::drawCone(int x, int y, int from, double degrees, int reach, Uint8 colour)
{
	const double middle = degrees * Pi / 180.0, half = ConeHalf * Pi / 180.0;
	for (int dy = -reach; dy <= reach; ++dy)
	{
		for (int dx = -reach; dx <= reach; ++dx)
		{
			const int far = dx * dx + dy * dy;
			// (every other square of the screen's magnification: the map shows through the same on any screen)
			if (far > reach * reach || far < from * from || (((x + dx) / _ui + (y + dy) / _ui) & 1)) continue;
			double off = std::atan2((double)dy, (double)dx) - middle;
			while (off > Pi) off -= 2.0 * Pi;
			while (off < -Pi) off += 2.0 * Pi;
			if (std::abs(off) <= half) _view->setPixel(x + dx, y + dy, colour);
		}
	}
	for (double side : { middle - half, middle + half })
	{
		_view->drawLine((Sint16)(x + std::lround(from * std::cos(side))), (Sint16)(y + std::lround(from * std::sin(side))),
			(Sint16)(x + std::lround(reach * std::cos(side))), (Sint16)(y + std::lround(reach * std::sin(side))), colour);
	}
}

/**
 * Draws a unit's dot: a disc with a black rim, or a ring (a unit on another level than the one looked at).
 * @param x, y Its middle.
 * @param radius Its radius.
 * @param colour Its colour.
 * @param ring Only its rim.
 */
void FirstPersonMapState::drawDisc(int x, int y, int radius, Uint8 colour, bool ring)
{
	for (int dy = -radius - 1; dy <= radius + 1; ++dy)
	{
		for (int dx = -radius - 1; dx <= radius + 1; ++dx)
		{
			const double far = std::sqrt((double)(dx * dx + dy * dy));
			if (far > radius + 1.3) continue;
			if (far > radius + 0.3) _view->setPixel(x + dx, y + dy, _black);
			else if (!ring || far > radius - 1.2) _view->setPixel(x + dx, y + dy, colour);
			else _view->setPixel(x + dx, y + dy, _black);
		}
	}
}

/**
 * What a unit holds, in a few letters: its right hand's weapon, else its left's; a name of nine letters or fewer
 * whole, a longer one its words' first letters and its last word ("H.Plasma" for a Heavy Plasma). An enemy's only if
 * the game would name the item (researched): nothing is told that the game doesn't tell.
 * @param unit The unit.
 * @param whole The whole name, however long (the line along the bottom).
 * @return The letters, or nothing.
 */
std::string FirstPersonMapState::weaponOf(const BattleUnit *unit, bool whole) const
{
	const BattleItem *item = unit->getRightHandWeapon() ? unit->getRightHandWeapon() : unit->getLeftHandWeapon();
	if (!item) return std::string();
	if (unit->getFaction() != FACTION_PLAYER && _game->getSavedGame() && !_game->getSavedGame()->isResearched(item->getRules()->getRequirements())) return std::string();
	const std::string name = tr(item->getRules()->getName());
	const UString letters = Unicode::convUtf8ToUtf32(name);
	if (whole || letters.size() <= 9) return name;
	UString brief;
	size_t from = 0;
	for (size_t space = letters.find(' '); space != UString::npos; space = letters.find(' ', from))
	{
		if (space > from) { brief += letters[from]; brief += '.'; }
		from = space + 1;
	}
	brief += letters.substr(from, 7);
	return Unicode::convUtf32ToUtf8(brief);
}

/**
 * What the line along the bottom says of a unit, piece by piece, each in its colour (the owner: "soldier name,
 * health, TU remaining and his/hers weapon - all in colored text as in hud"). One of ours: its number and name, its
 * health, its time units and the weapon in hand, each in the colour the 3D view's panel has it in. Anyone else: its
 * name in its dot's colour, what it holds if the game would name it, and how far it is from the selected soldier.
 * Anyone on another level than the one looked at: which.
 * @param unit The unit.
 * @param brief How much about the weapon: 0 as the 3D view's panel words it, with its rounds; 1 its name; 2 the few
 *   letters under the dots.
 * @return The pieces, left to right.
 */
std::vector<FirstPersonMapState::Piece> FirstPersonMapState::statusOf(BattleUnit *unit, int brief) const
{
	std::vector<Piece> pieces;
	const bool ours = unit->getFaction() == FACTION_PLAYER;
	std::string name = unit->getName(_game->getLanguage());
	if (const int number = numberOf(unit)) name = std::to_string(number) + " " + name;
	pieces.push_back({ name, ours ? _nameColour : unit->getFaction() == FACTION_HOSTILE ? _enemy : _other });
	if (ours)
	{
		// (an overkill leaves the health below zero)
		pieces.push_back({ std::string(tr("STR_FPS_TMAP_HEALTH").arg(std::max(0, unit->getHealth())).arg(unit->getBaseStats()->health)), _healthColour });
		pieces.push_back({ std::string(tr("STR_FPS_TMAP_TIME").arg(unit->getTimeUnits()).arg(unit->getBaseStats()->tu)), _timeColour });
	}
	const std::string weapon = brief < 2 && ours && _setup.weaponWords ? _setup.weaponWords(unit, brief == 0) : weaponOf(unit, brief < 2);
	if (!weapon.empty()) pieces.push_back({ weapon, _weaponColour });
	const BattleUnit *selected = _save->getSelectedUnit();
	if (!ours && selected && selected != unit && !selected->isOut())
	{
		const int dx = unit->getPosition().x - selected->getPosition().x, dy = unit->getPosition().y - selected->getPosition().y;
		pieces.push_back({ std::string(tr("STR_FPS_TMAP_AWAY").arg((int)std::lround(std::sqrt((double)(dx * dx + dy * dy))))), _white });
	}
	if (unit->getPosition().z != _level) pieces.push_back({ std::string(tr("STR_FPS_TMAP_ON_LEVEL").arg(unit->getPosition().z + 1)), _grey });
	return pieces;
}

/**
 * Works out the selected soldier's reach, if it isn't known for them as they are now: every tile the game's own
 * pathfinding says they can walk (or fly) to with the time units and the energy they have, on any level (the
 * owner: "a reach based on remaining TUs of the unit ... based on exact current AVAILABLE TUs"). In your turn, for
 * one of yours; with the reach switched off, none.
 */
void FirstPersonMapState::updateReach()
{
	const BattleUnit *unit = _save->getSelectedUnit();
	if (!Reach || !unit || unit->isOut() || unit->getFaction() != FACTION_PLAYER || _save->getSide() != FACTION_PLAYER)
	{
		_reachUnit = nullptr;
		_reach.clear();
		return;
	}
	if (unit == _reachUnit && unit->getTimeUnits() == _reachTime && unit->getEnergy() == _reachEnergy && unit->getPosition() == _reachFrom) return;
	_reachUnit = unit;
	_reachTime = unit->getTimeUnits();
	_reachEnergy = unit->getEnergy();
	_reachFrom = unit->getPosition();
	const int wide = _save->getMapSizeX(), high = _save->getMapSizeY(), tiles = unit->getArmor()->getSize();
	_reach.assign((size_t)wide * high, 0);
	for (int index : _save->getPathfinding()->findReachable(unit, BattleActionCost()))
	{
		const Position at = _save->getTileCoords(index);
		for (int y = at.y; y < at.y + tiles && y < high; ++y)
			for (int x = at.x; x < at.x + tiles && x < wide; ++x)
				if (x >= 0 && y >= 0) _reach[(size_t)y * wide + x] = 1;
	}
	_dirty = true;
}

/**
 * Draws the selected soldier's reach: a line in the selected soldier's blue along every side of a tile they can
 * reach that has a tile they can't beside it.
 * @param originX, originY Where the map's top left corner is on the screen.
 */
void FirstPersonMapState::drawReach(int originX, int originY)
{
	if (_reach.empty()) return;
	const int wide = _save->getMapSizeX(), high = _save->getMapSizeY(), size = cell(), thick = _ui;
	auto reached = [&](int x, int y) { return x >= 0 && y >= 0 && x < wide && y < high && _reach[(size_t)y * wide + x]; };
	const int first[2] = { std::max(0, -originX / size - 1), std::max(0, -originY / size - 1) };
	const int last[2] = { std::min(wide - 1, (_view->getWidth() - originX) / size + 1), std::min(high - 1, (_view->getHeight() - originY) / size + 1) };
	for (int y = first[1]; y <= last[1]; ++y)
	{
		for (int x = first[0]; x <= last[0]; ++x)
		{
			if (!reached(x, y)) continue;
			const int left = originX + x * size, top = originY + y * size;
			if (!reached(x, y - 1)) _view->drawRect((Sint16)left, (Sint16)top, (Sint16)size, (Sint16)thick, _selected);
			if (!reached(x, y + 1)) _view->drawRect((Sint16)left, (Sint16)(top + size - thick), (Sint16)size, (Sint16)thick, _selected);
			if (!reached(x - 1, y)) _view->drawRect((Sint16)left, (Sint16)top, (Sint16)thick, (Sint16)size, _selected);
			if (!reached(x + 1, y)) _view->drawRect((Sint16)(left + size - thick), (Sint16)top, (Sint16)thick, (Sint16)size, _selected);
		}
	}
}

/**
 * Points the helmet camera, every frame: through the eyes of the soldier the pointer is on, if one of yours, else
 * the selected soldier's; turned to an enemy the pointer is on. With the camera switched off, or nobody to look
 * through, it hides. (The owner: "one can click on dot and see the units 3d view also?")
 */
void FirstPersonMapState::updateHelmet()
{
	if (!_helmet) return;
	const Dot *pointed = buttonAt(_mouseX, _mouseY) ? nullptr : dotAt(_mouseX, _mouseY);
	BattleUnit *through = pointed && pointed->unit->getFaction() == FACTION_PLAYER ? pointed->unit : _save->getSelectedUnit();
	const BattleUnit *at = pointed && pointed->unit->getFaction() != FACTION_PLAYER ? pointed->unit : nullptr;
	if (!Helmet || !through || through->isOut() || through->getFaction() != FACTION_PLAYER)
	{
		if (_helmet->getVisible()) _dirty = true;
		_helmet->setVisible(false);
		_helmetUnit = nullptr;
		return;
	}
	if (through != _helmetUnit || at != _helmetLooksAt || !_helmet->getVisible()) _dirty = true; // (its title changes)
	_helmetUnit = through;
	_helmetLooksAt = at;
	_helmet->setVisible(true);
	_setup.aimCamera(_helmet, through, at);
}

/**
 * Draws the map: the tiles enlarged, the items on the floors, the selected soldier's reach, the cones, the dots, the
 * names and the weapons, then the buttons, the level and the turn along the top, the helmet camera's frame, and
 * along the bottom the line about the unit the pointer is on, else the selected soldier (see statusOf), with the help
 * over it while that is switched on (a button's use, or how the map is worked).
 */
void FirstPersonMapState::draw()
{
	_dirty = false;
	if (_terrainLevel != _level) rebuildTerrain();
	const int width = _view->getWidth(), height = _view->getHeight(), zoom = _zoom, size = cell();
	const int mapWide = _save->getMapSizeX() * CellPixels, mapHigh = _save->getMapSizeY() * CellPixels;
	const int originX = (int)std::lround(width / 2.0 - _centreX * size), originY = (int)std::lround(height / 2.0 - _centreY * size);
	// The tiles: each pixel of the little pictures a square of the zoom's side; off the map, a dark blue.
	_view->lock();
	_terrain->lock();
	{
		Uint8 *out = _view->getBuffer();
		const Uint8 *in = _terrain->getBuffer();
		const int pitch = _view->getPitch(), inPitch = _terrain->getPitch();
		for (int y = 0; y < height; ++y)
		{
			Uint8 *row = out + (size_t)y * pitch;
			const int fromTop = y - originY, mapRow = fromTop >= 0 ? fromTop / zoom : -1;
			if (mapRow < 0 || mapRow >= mapHigh)
			{
				std::memset(row, _dark, (size_t)width);
				continue;
			}
			const Uint8 *source = in + (size_t)mapRow * inPitch;
			for (int x = 0; x < width; ++x)
			{
				const int fromLeft = x - originX, mapColumn = fromLeft >= 0 ? fromLeft / zoom : -1;
				row[x] = mapColumn < 0 || mapColumn >= mapWide ? _dark : source[mapColumn] ? source[mapColumn] : _black;
			}
		}
	}
	_terrain->unlock();
	_view->unlock();

	// The items lying about (on a floor seen, the levels up to the one looked at; not those a mod hides from the map):
	// a small square that blinks, red with a fuse burning.
	const int first[2] = { std::max(0, -originX / size), std::max(0, -originY / size) };
	const int last[2] = { std::min(_save->getMapSizeX() - 1, (width - originX) / size), std::min(_save->getMapSizeY() - 1, (height - originY) / size) };
	for (int level = 0; level <= _level && (_frame / 2) % 2 == 0; ++level)
	{
		for (int y = first[1]; y <= last[1]; ++y)
		{
			for (int x = first[0]; x <= last[0]; ++x)
			{
				Tile *tile = _save->getTile(Position(x, y, level));
				if (!tile || !tile->isDiscovered(O_FLOOR) || tile->getInventory()->empty()) continue;
				bool any = false, primed = false;
				for (const BattleItem *item : *tile->getInventory())
				{
					if (item->getRules()->isHiddenOnMinimap()) continue;
					any = true;
					primed = primed || item->getFuseTimer() >= 0;
				}
				if (!any) continue;
				const int side = std::max(2 * _ui, size / 3);
				_view->drawRect((Sint16)(originX + x * size + (size - side) / 2), (Sint16)(originY + y * size + (size - side) / 2), (Sint16)side, (Sint16)side, primed ? _enemy : _item);
			}
		}
	}
	drawReach(originX, originY);

	// The units: where each one's dot is, then the cones under the dots, the dots, and the names over all. (A unit two
	// tiles by two, a tank, has a dot twice as big: it covers its four tiles.)
	_dots.clear();
	const BattleUnit *selected = _save->getSelectedUnit();
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (!shows(unit)) continue;
		const int tiles = unit->getArmor()->getSize();
		Dot dot;
		dot.unit = unit;
		dot.x = originX + (int)std::lround((unit->getPosition().x + tiles * 0.5) * size);
		dot.y = originY + (int)std::lround((unit->getPosition().y + tiles * 0.5) * size);
		dot.radius = std::clamp(size * 3 / 8, 2 * _ui, 7 * _ui) * (tiles > 1 ? 2 : 1);
		if (dot.x < -big(40) || dot.y < -big(40) || dot.x > width + big(40) || dot.y > height + big(40)) continue;
		_dots.push_back(dot);
	}
	auto colourOf = [&](const BattleUnit *unit) {
		return unit == selected ? _selected : unit->getFaction() == FACTION_PLAYER ? _ours : unit->getFaction() == FACTION_HOSTILE ? _enemy : _other;
	};
	_view->lock();
	for (const Dot &dot : _dots)
	{
		if (!Cones) break;
		// (the selected soldier's the way the 3D view looks, which turns freely; anyone else's the way they face)
		const double degrees = dot.unit == selected && _setup.hasViewYaw ? _setup.viewYawDegrees : (dot.unit->getDirection() - 2) * 45.0;
		drawCone(dot.x, dot.y, dot.radius + 1, degrees, dot.radius + std::max(big(9), size * 5 / 2), colourOf(dot.unit));
	}
	for (const Dot &dot : _dots)
	{
		drawDisc(dot.x, dot.y, dot.radius, colourOf(dot.unit), dot.unit->getPosition().z != _level);
		// (the selected soldier's dot beats: a white rim every other step)
		if (dot.unit == selected && (_frame / 2) % 2 == 0)
		{
			const int rim = dot.radius + 2 * _ui, steps = 16 * rim;
			for (int step = 0; step < steps; ++step)
			{
				const double turn = step * 2.0 * Pi / steps;
				_view->setPixel(dot.x + (int)std::lround(rim * std::cos(turn)), dot.y + (int)std::lround(rim * std::sin(turn)), _white);
			}
		}
	}
	_view->unlock();
	// The names: one of ours its number, anyone else its kind's letters; on another level, a mark for above or below.
	// Each beside its dot where it covers no dot and no name written before it (to the right, the left, above, below:
	// the first that is free; the selected soldier's first, then ours, then the others'). The weapons: under the dots
	// (else over them), where that is free; left out where it is not.
	std::vector<SDL_Rect> taken;
	auto free = [&](const SDL_Rect &box) {
		for (const SDL_Rect &other : taken)
			if (box.x < other.x + other.w && other.x < box.x + box.w && box.y < other.y + other.h && other.y < box.y + box.h) return false;
		return true;
	};
	auto boxOf = [&](int x, int y, int wide) {
		const SDL_Rect box = { (Sint16)(x - _ui), (Sint16)(y - _ui), (Uint16)(wide + 2 * _ui), (Uint16)(big(WordsHigh) + _ui) };
		return box;
	};
	for (const Dot &dot : _dots)
	{
		const SDL_Rect box = { (Sint16)(dot.x - dot.radius - 1), (Sint16)(dot.y - dot.radius - 1), (Uint16)(2 * dot.radius + 3), (Uint16)(2 * dot.radius + 3) };
		taken.push_back(box);
	}
	for (int turn = 0; turn < 3 && Names; ++turn)
	{
		for (const Dot &dot : _dots)
		{
			const int which = dot.unit == selected ? 0 : dot.unit->getFaction() == FACTION_PLAYER ? 1 : 2;
			if (which != turn) continue;
			const int number = dot.unit->getFaction() == FACTION_PLAYER ? numberOf(dot.unit) : 0;
			std::string name = number ? std::to_string(number) : kindOf(dot.unit);
			if (dot.unit->getPosition().z > _level) name += "^";
			else if (dot.unit->getPosition().z < _level) name += "v";
			_words->setText(name);
			const int wide = big(_words->getTextWidth()), reach = dot.radius + 3 * _ui, high = big(WordsHigh);
			const int places[4][2] = { { dot.x + reach, dot.y - high / 2 }, { dot.x - reach - wide, dot.y - high / 2 },
				{ dot.x - wide / 2, dot.y - reach - high }, { dot.x - wide / 2, dot.y + reach } };
			int chosen = 0;
			for (int place = 0; place < 4; ++place)
			{
				if (free(boxOf(places[place][0], places[place][1], wide))) { chosen = place; break; }
			}
			taken.push_back(boxOf(places[chosen][0], places[chosen][1], wide));
			write(name, places[chosen][0], places[chosen][1], dot.unit == selected ? _white : colourOf(dot.unit), true);
		}
	}
	for (int turn = 0; turn < 3 && Weapons; ++turn)
	{
		for (const Dot &dot : _dots)
		{
			const int which = dot.unit == selected ? 0 : dot.unit->getFaction() == FACTION_PLAYER ? 1 : 2;
			if (which != turn) continue;
			const std::string weapon = weaponOf(dot.unit);
			if (weapon.empty()) continue;
			_words->setText(weapon);
			const int wide = big(_words->getTextWidth()), reach = dot.radius + 3 * _ui, high = big(WordsHigh);
			const int places[2][2] = { { dot.x - wide / 2, dot.y + reach }, { dot.x - wide / 2, dot.y - reach - high } };
			for (const int *place : places)
			{
				if (!free(boxOf(place[0], place[1], wide))) continue;
				taken.push_back(boxOf(place[0], place[1], wide));
				write(weapon, place[0], place[1], _item, true);
				break;
			}
		}
	}

	// The buttons (the one under the pointer lit; those that show or hide something grey when it is hidden), then the
	// level and the turn: after the last button where the row has room for them, else under the buttons.
	const Button *under = buttonAt(_mouseX, _mouseY);
	int lastRight = big(ButtonGap), lastTop = big(ButtonGap);
	for (const Button &button : _buttons)
	{
		const bool off = (button.kind == BUTTON_NAMES && !Names) || (button.kind == BUTTON_CONES && !Cones) || (button.kind == BUTTON_WEAPONS && !Weapons)
			|| (button.kind == BUTTON_REACH && !Reach) || (button.kind == BUTTON_CAMERA && !Helmet) || (button.kind == BUTTON_HELP && !Help);
		_view->drawRect((Sint16)button.box.x, (Sint16)button.box.y, (Sint16)button.box.w, (Sint16)button.box.h, &button == under ? _white : _edge);
		_view->drawRect((Sint16)(button.box.x + _ui), (Sint16)(button.box.y + _ui), (Sint16)(button.box.w - 2 * _ui), (Sint16)(button.box.h - 2 * _ui), _panel);
		_words->setText(button.words);
		write(button.words, button.box.x + (button.box.w - big(_words->getTextWidth())) / 2, button.box.y + (big(ButtonHigh) - big(WordsHigh)) / 2 + _ui, off ? _grey : _white, false);
		if (button.kind != BUTTON_CLOSE && button.kind != BUTTON_HELP && (button.box.y > lastTop || (button.box.y == lastTop && button.box.x + button.box.w > lastRight)))
		{
			lastTop = button.box.y;
			lastRight = button.box.x + button.box.w;
		}
	}
	const std::string level = tr("STR_FPS_TMAP_LEVEL").arg(_level + 1).arg(_save->getMapSizeZ()), turn = tr("STR_FPS_TMAP_TURN").arg(_save->getTurn());
	_words->setText(level + "   " + turn);
	const int rowEnd = lastTop == big(ButtonGap) ? _cornerLeft : width;
	const bool beside = lastRight + 3 * big(ButtonGap) + big(_words->getTextWidth()) + 3 * big(ButtonGap) < rowEnd;
	const int wordsX = beside ? lastRight + 3 * big(ButtonGap) : big(ButtonGap) + _ui;
	const int wordsY = beside ? lastTop + (big(ButtonHigh) - big(WordsHigh)) / 2 + _ui : lastTop + big(ButtonHigh) + big(ButtonGap) + _ui;
	const int levelWide = write(level, wordsX, wordsY, _white, true);
	write(turn, wordsX + levelWide + 3 * big(ButtonGap), wordsY, _grey, true);

	// The helmet camera's frame, and over it whose eyes it is (and whom they look at).
	if (_helmet && _helmet->getVisible() && _helmetUnit)
	{
		const int x = _helmet->getX(), y = _helmet->getY(), wide = _helmet->getWidth(), high = _helmet->getHeight();
		_view->drawRect((Sint16)(x - _ui), (Sint16)(y - _ui - big(WordsHigh) - _ui), (Sint16)(wide + 2 * _ui), (Sint16)(high + 2 * _ui + big(WordsHigh) + _ui), _edge);
		_view->drawRect((Sint16)x, (Sint16)(y - big(WordsHigh) - _ui), (Sint16)wide, (Sint16)(big(WordsHigh) + _ui), _panel);
		std::string title = _helmetUnit->getName(_game->getLanguage());
		if (const int number = numberOf(_helmetUnit)) title = std::to_string(number) + " " + title;
		if (_helmetLooksAt) title += " > " + kindOf(_helmetLooksAt);
		write(title, x + _ui, y - big(WordsHigh), _white, false);
	}

	// The line along the bottom: the unit the pointer is on, else the selected soldier, piece by piece in their colours
	// on one dark plate (see statusOf), with as much about the weapon as the screen has room for.
	const int left = big(ButtonGap) + _ui, room = width - 2 * left, rowHigh = big(WordsHigh) + _ui, bottom = height - big(WordsHigh) - big(ButtonGap);
	const Dot *pointed = under ? nullptr : dotAt(_mouseX, _mouseY);
	BattleUnit *told = pointed ? pointed->unit : _save->getSelectedUnit();
	if (shows(told))
	{
		std::vector<Piece> pieces;
		int wide = 0;
		for (int brief = 0; brief < 3; ++brief)
		{
			pieces = statusOf(told, brief);
			wide = -big(StatusGap);
			for (const Piece &piece : pieces) wide += wideOf(piece.words) + big(StatusGap);
			if (wide <= room) break;
		}
		_view->drawRect((Sint16)(left - _ui), (Sint16)(bottom - _ui), (Sint16)(std::min(wide, room) + 2 * _ui), (Sint16)rowHigh, _panel);
		int x = left;
		for (const Piece &piece : pieces)
		{
			x += write(piece.words, x, bottom, piece.colour, false) + big(StatusGap);
		}
	}
	// The help, over that line while it is switched on: what the button under the pointer does, else how the map is
	// worked; in as many rows as it needs here, the last right over the line.
	if (Help)
	{
		const std::vector<std::string> rows = wrapped(under ? under->tip : _hint, room);
		for (size_t row = 0; row < rows.size(); ++row)
		{
			write(rows[row], left, bottom - (int)(rows.size() - row) * rowHigh, under ? _white : _grey, true);
		}
	}
}

/**
 * The dot the pointer is on (the nearest, within DotReach of its rim).
 * @param x, y The pointer.
 * @return The dot, or null.
 */
const FirstPersonMapState::Dot *FirstPersonMapState::dotAt(int x, int y) const
{
	const Dot *best = nullptr;
	int bestFar = INT_MAX;
	for (const Dot &dot : _dots)
	{
		const int far = (dot.x - x) * (dot.x - x) + (dot.y - y) * (dot.y - y), reach = dot.radius + big(DotReach);
		if (far <= reach * reach && far < bestFar)
		{
			best = &dot;
			bestFar = far;
		}
	}
	return best;
}

/**
 * The button the pointer is on.
 * @param x, y The pointer.
 * @return The button, or null.
 */
const FirstPersonMapState::Button *FirstPersonMapState::buttonAt(int x, int y) const
{
	for (const Button &button : _buttons)
	{
		if (x >= button.box.x && y >= button.box.y && x < button.box.x + button.box.w && y < button.box.y + button.box.h) return &button;
	}
	return nullptr;
}

/**
 * A button pressed.
 * @param kind Which.
 */
void FirstPersonMapState::press(ButtonKind kind)
{
	switch (kind)
	{
	case BUTTON_LEVEL_UP: changeLevel(1); break;
	case BUTTON_LEVEL_DOWN: changeLevel(-1); break;
	case BUTTON_ZOOM_OUT: changeZoom(-1); break;
	case BUTTON_ZOOM_IN: changeZoom(1); break;
	case BUTTON_CENTRE:
		if (const BattleUnit *selected = _save->getSelectedUnit())
		{
			centreOn(selected);
			_level = std::clamp((int)selected->getPosition().z, 0, _save->getMapSizeZ() - 1);
		}
		break;
	case BUTTON_NEXT: nextSoldier(1); break;
	case BUTTON_NAMES: Names = !Names; break;
	case BUTTON_WEAPONS: Weapons = !Weapons; break;
	case BUTTON_CONES: Cones = !Cones; break;
	case BUTTON_REACH: Reach = !Reach; break;
	case BUTTON_CAMERA: Helmet = !Helmet; break;
	case BUTTON_HELP:
		Help = !Help;
		placeHelmet(); // (the help's rows are under the camera's picture)
		break;
	case BUTTON_CLOSE: close(); return;
	}
	_dirty = true;
}

/**
 * Looks a level up or down (the game's map: its two buttons; its wheel is the zoom's here, the level's with Ctrl).
 * @param by Levels up.
 */
void FirstPersonMapState::changeLevel(int by)
{
	_level = std::clamp(_level + by, 0, _save->getMapSizeZ() - 1);
	_dirty = true;
}

/**
 * Enlarges the map a step, or makes it a step smaller; the middle stays where it is.
 * @param by Steps bigger.
 */
void FirstPersonMapState::changeZoom(int by)
{
	_zoom = std::clamp(_zoom + by, 1, ZoomMost * _ui);
	if (!embedded())
	{
		Zoom = _zoom;
		Options::fpsMapZoom = Zoom; // (kept with the options: see Zoom)
	}
	_dirty = true;
}

/**
 * Enlarges the map a step, or makes it a step smaller, about a place on the screen: the tile there stays there (the
 * wheel: the tile under the pointer; the owner: "the mouse wheel should not scroll levels of the map up down but zoom
 * the map in/out").
 * @param by Steps bigger.
 * @param x, y The place on the screen.
 */
void FirstPersonMapState::zoomAt(int by, int x, int y)
{
	const double offX = x - _view->getWidth() / 2.0, offY = y - _view->getHeight() / 2.0;
	const double tileX = _centreX + offX / cell(), tileY = _centreY + offY / cell();
	changeZoom(by);
	_centreX = tileX - offX / cell();
	_centreY = tileY - offY / cell();
}

/**
 * Puts a unit in the middle of the screen.
 * @param unit The unit.
 */
void FirstPersonMapState::centreOn(const BattleUnit *unit)
{
	_centreX = unit->getPosition().x + unit->getArmor()->getSize() * 0.5;
	_centreY = unit->getPosition().y + unit->getArmor()->getSize() * 0.5;
	_dirty = true;
}

/**
 * Selects the next soldier of the squad who is up (or the one before), and puts them in the middle, on their level.
 * @param by 1 the next, -1 the one before.
 */
void FirstPersonMapState::nextSoldier(int by)
{
	const int count = (int)_setup.squad.size();
	if (!count || !_setup.select) return;
	const auto now = std::find(_setup.squad.begin(), _setup.squad.end(), _save->getSelectedUnit());
	int place = now != _setup.squad.end() ? (int)(now - _setup.squad.begin()) : (by > 0 ? -1 : 0);
	for (int tried = 0; tried < count; ++tried)
	{
		place = ((place + by) % count + count) % count;
		BattleUnit *unit = _setup.squad[place];
		if (!unit || unit->isOut() || unit->getFaction() != FACTION_PLAYER) continue;
		_setup.select(unit);
		_setup.hasViewYaw = false; // (the view was another soldier's: everyone's cone is the way they face now)
		if (_save->getSelectedUnit() == unit)
		{
			centreOn(unit);
			_level = std::clamp((int)unit->getPosition().z, 0, _save->getMapSizeZ() - 1);
		}
		break;
	}
	_dirty = true;
}

/**
 * Closes the map: the battle's own view is left looking where the map did, on its level (as the game's map leaves it).
 */
void FirstPersonMapState::close()
{
	if (embedded()) return; // (in a window it never closes: see Setup::window)
	_camera->setViewLevel(_level);
	_camera->centerOnPosition(Position((int)std::floor(_centreX), (int)std::floor(_centreY), _level), false);
	_game->popState();
}

/**
 * A button of the mouse goes down on the map: a drag may begin (the left button, or the game's own drag button).
 * @param action The press.
 */
void FirstPersonMapState::viewPress(Action *action)
{
	const Uint8 button = action->getDetails()->button.button;
	if (button != SDL_BUTTON_LEFT && button != Options::battleDragScrollButton) return;
	_mouseX = (int)std::floor(action->getRelativeXMouse() / action->getXScale());
	_mouseY = (int)std::floor(action->getRelativeYMouse() / action->getYScale());
	if (buttonAt(_mouseX, _mouseY)) return; // (a button: its release presses it)
	_pressed = true;
	_dragged = false;
	_pressX = _mouseX;
	_pressY = _mouseY;
	_pressCentreX = _centreX;
	_pressCentreY = _centreY;
}

/**
 * A button of the mouse comes up. The left one, not dragged: on a button, the button; on one of ours, that soldier
 * is selected; anywhere else, the map is centred there (the game's map's click). The right one, not dragged: the
 * map closes (the game's map's too).
 * @param action The release.
 */
void FirstPersonMapState::viewRelease(Action *action)
{
	const Uint8 button = action->getDetails()->button.button;
	const bool dragged = _pressed && _dragged;
	if (button == SDL_BUTTON_LEFT || button == Options::battleDragScrollButton) _pressed = false;
	if (dragged) return;
	const int x = (int)std::floor(action->getRelativeXMouse() / action->getXScale()), y = (int)std::floor(action->getRelativeYMouse() / action->getYScale());
	if (button == SDL_BUTTON_RIGHT)
	{
		close();
		return;
	}
	if (button != SDL_BUTTON_LEFT) return;
	if (const Button *pressed = buttonAt(x, y))
	{
		press(pressed->kind);
		return;
	}
	const Dot *dot = dotAt(x, y);
	if (dot && dot->unit->getFaction() == FACTION_PLAYER && _setup.select && numberOf(dot->unit))
	{
		// (the selected soldier clicked: into their 3D view; another: selected, a second click goes to theirs)
		if (dot->unit == _save->getSelectedUnit())
		{
			close();
			return;
		}
		_setup.hasViewYaw = false; // (see nextSoldier)
		_setup.select(dot->unit);
	}
	else
	{
		_centreX += (x - _view->getWidth() / 2.0) / cell();
		_centreY += (y - _view->getHeight() / 2.0) / cell();
	}
	_dirty = true;
}

/**
 * The pointer moves over the map: a drag scrolls it; else the unit under it is told of along the bottom (and the
 * button under it by the help).
 * @param action The move.
 */
void FirstPersonMapState::viewOver(Action *action)
{
	const int x = (int)std::floor(action->getRelativeXMouse() / action->getXScale()), y = (int)std::floor(action->getRelativeYMouse() / action->getYScale());
	if (x == _mouseX && y == _mouseY) return;
	_mouseX = x;
	_mouseY = y;
	// (a release missed, the pointer off the window: no button is down any more)
	if (_pressed && !(SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(Options::battleDragScrollButton)))) _pressed = false;
	if (_pressed)
	{
		if (std::abs(x - _pressX) > big(DragStart) || std::abs(y - _pressY) > big(DragStart)) _dragged = true;
		if (_dragged)
		{
			_centreX = _pressCentreX - (double)(x - _pressX) / cell();
			_centreY = _pressCentreY - (double)(y - _pressY) / cell();
		}
	}
	_dirty = true;
}

/**
 * The wheel and the keys. The wheel: the map bigger or smaller about the tile under the pointer; with Ctrl a level up
 * or down (the game's map's wheel); the other way round with the setting "Tactical map: wheel changes level". Keys: Escape, the map's key or OK closes; the arrows scroll; the game's level
 * keys, Page Up and Page Down change the level; + and - zoom; Home (or the game's key that centres on the unit) puts
 * the selected soldier in the middle; the game's next and previous unit keys select them; F1 the help.
 * @param action What happened.
 */
void FirstPersonMapState::handle(Action *action)
{
	State::handle(action);
	const SDL_Event *event = action->getDetails();
	if (event->type == SDL_MOUSEBUTTONDOWN && (event->button.button == SDL_BUTTON_WHEELUP || event->button.button == SDL_BUTTON_WHEELDOWN))
	{
		const int by = event->button.button == SDL_BUTTON_WHEELUP ? 1 : -1;
		// (where the wheel turned, as the view was told of it; never told: where the pointer was last, else the middle)
		// (on the map's own surface: in a window, off its corner)
		const int x = action->getAbsoluteXMouse() >= 0 ? (int)std::floor(action->getAbsoluteXMouse()) - _view->getX() : _mouseX;
		const int y = action->getAbsoluteYMouse() >= 0 ? (int)std::floor(action->getAbsoluteYMouse()) - _view->getY() : _mouseY;
		// (the setting "Tactical map: wheel changes level" swaps the two: a player, GitHub issue #215)
		if (_game->isCtrlPressed() != Options::fpsMapWheelLevels) changeLevel(by);
		else if (x >= 0 && y >= 0) zoomAt(by, x, y);
		else changeZoom(by);
	}
	else if (event->type == SDL_KEYDOWN && !embedded()) // (in a window the keys are the battle's)
	{
		const SDLKey key = event->key.keysym.sym;
		const double step = std::max(1.0, big(24) / (double)cell());
		if (key == Options::keyCancel || key == Options::keyBattleMap || key == Options::keyOk) { close(); return; }
		else if (key == SDLK_LEFT) _centreX -= step;
		else if (key == SDLK_RIGHT) _centreX += step;
		else if (key == SDLK_UP) _centreY -= step;
		else if (key == SDLK_DOWN) _centreY += step;
		else if (key == SDLK_PAGEUP || key == Options::keyBattleLevelUp) changeLevel(1);
		else if (key == SDLK_PAGEDOWN || key == Options::keyBattleLevelDown) changeLevel(-1);
		else if (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_KP_PLUS) changeZoom(1);
		else if (key == SDLK_MINUS || key == SDLK_KP_MINUS) changeZoom(-1);
		else if (key == SDLK_HOME || key == Options::keyBattleCenterUnit) press(BUTTON_CENTRE);
		else if (key == Options::keyBattleNextUnit) nextSoldier(1);
		else if (key == Options::keyBattlePrevUnit) nextSoldier(-1);
		else if (key == SDLK_F1) press(BUTTON_HELP);
		_dirty = true;
	}
}

/**
 * The animation's step, eight times a second: every other one the items blink and the selected soldier's dot beats.
 */
void FirstPersonMapState::animate()
{
	_frame = (_frame + 1) % 8;
	if (_frame % 2 == 0 || embedded()) _dirty = true; // (in a window the battle goes on under it: the units move)
}

/**
 * Draws the map onto a surface of the screen's size, as State::blit draws it onto the game's screen: in a window, onto
 * the Live command feed's own screen when the game's is in full colour (see FirstPersonMode::blitFeed).
 * @param target The surface.
 */
void FirstPersonMapState::blitOnto(SDL_Surface *target)
{
	for (Surface *surface : _surfaces)
	{
		surface->blit(target);
	}
}

/**
 * Every frame: the clock, and the map drawn again if anything changed.
 */
void FirstPersonMapState::think()
{
	State::think();
	_clock->think(this, 0);
	// In a window: the soldier selected now in the middle, on their level, each time another is selected (ours only:
	// in the aliens' turn the alien acting is the selected unit, and it may be one nobody sees).
	const BattleUnit *selected = _save->getSelectedUnit();
	if (embedded() && selected != _followed && selected && selected->getFaction() == FACTION_PLAYER && !selected->isOut() && selected->getTile())
	{
		_followed = selected;
		centreOn(selected);
		_level = std::clamp((int)selected->getPosition().z, 0, _save->getMapSizeZ() - 1);
	}
	updateReach();
	updateHelmet();
	if (_dirty) draw();
}

}
