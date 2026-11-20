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
#include "../Battlescape/Camera.h"
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
#include "../Savegame/Tile.h"

namespace OpenXcom
{

namespace
{

/// A tile's little picture is this many pixels square (the game's SCANG.DAT), and its number there is the part's
/// map number and this (see MiniMapView).
const int CellPixels = 4, TilePictures = 35;
/// The map is enlarged by whole steps, 1 to ZoomMost; what the player last chose is kept for the next time, with the
/// names and the cones on or off (0: not chosen yet, the map picks what suits the screen).
const int ZoomMost = 8;
int Zoom = 0;
bool Names = true, Cones = true;
/// A cone opens this many degrees either side of the way its unit faces (the game's units see a quarter circle).
const double ConeHalf = 45.0;
/// The pointer is on a dot within this many pixels of its rim; a press that moves further than DragStart is a drag.
const int DotReach = 3, DragStart = 3;
/// The buttons: this high, this far apart and from the screen's edges, their words this far inside; a line of words
/// is this high.
const int ButtonHigh = 13, ButtonGap = 2, ButtonPad = 4, WordsHigh = 9;

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
	const int width = Options::baseXResolution, height = Options::baseYResolution;
	_view = new InteractiveSurface(width, height, 0, 0);
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

	_level = std::clamp(camera->getViewLevel(), 0, save->getMapSizeZ() - 1);
	if (Zoom <= 0)
	{
		Zoom = std::clamp(std::min(width, height) / (40 * CellPixels), width >= 480 ? 2 : 1, ZoomMost);
	}
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
	return CellPixels * Zoom;
}

/**
 * Lays the buttons out along the top: the level's two, the zoom's two, the map back on the soldier, the next
 * soldier, the names and the cones; and OK in the top right corner. Each as wide as its words.
 */
void FirstPersonMapState::layOut()
{
	struct Said { ButtonKind kind; const char *words, *tip; };
	static const Said all[] = {
		{ BUTTON_LEVEL_UP, "STR_FPS_TMAP_UP", "STR_FPS_TMAP_UP_TIP" }, { BUTTON_LEVEL_DOWN, "STR_FPS_TMAP_DOWN", "STR_FPS_TMAP_DOWN_TIP" },
		{ BUTTON_ZOOM_OUT, "STR_FPS_TMAP_ZOOM_OUT", "STR_FPS_TMAP_ZOOM_OUT_TIP" }, { BUTTON_ZOOM_IN, "STR_FPS_TMAP_ZOOM_IN", "STR_FPS_TMAP_ZOOM_IN_TIP" },
		{ BUTTON_CENTRE, "STR_FPS_TMAP_CENTRE", "STR_FPS_TMAP_CENTRE_TIP" }, { BUTTON_NEXT, "STR_FPS_TMAP_NEXT", "STR_FPS_TMAP_NEXT_TIP" },
		{ BUTTON_NAMES, "STR_FPS_TMAP_NAMES", "STR_FPS_TMAP_NAMES_TIP" }, { BUTTON_CONES, "STR_FPS_TMAP_CONES", "STR_FPS_TMAP_CONES_TIP" },
		{ BUTTON_CLOSE, "STR_FPS_TMAP_CLOSE", "STR_FPS_TMAP_CLOSE_TIP" } };
	_buttons.clear();
	int x = ButtonGap;
	for (const Said &said : all)
	{
		Button button;
		button.kind = said.kind;
		button.words = tr(said.words);
		button.tip = tr(said.tip);
		_words->setText(button.words);
		const int wide = std::max(ButtonHigh, _words->getTextWidth() + 2 * ButtonPad);
		button.box.x = (Sint16)(said.kind == BUTTON_CLOSE ? _view->getWidth() - ButtonGap - wide : x);
		button.box.y = ButtonGap;
		button.box.w = (Uint16)wide;
		button.box.h = ButtonHigh;
		if (said.kind != BUTTON_CLOSE) x += wide + ButtonGap;
		// (a little apart after each pair: the level's, the zoom's, the soldiers')
		if (said.kind == BUTTON_LEVEL_DOWN || said.kind == BUTTON_ZOOM_IN || said.kind == BUTTON_NEXT) x += 3 * ButtonGap;
		_buttons.push_back(button);
	}
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
 * Writes words on the map.
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
	const int wide = _words->getTextWidth();
	if (backed) _view->drawRect((Sint16)(x - 1), (Sint16)(y - 1), (Sint16)(wide + 2), (Sint16)(WordsHigh + 1), _panel);
	_words->draw();
	_words->blitNShade(_view, x, y, 0);
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
			if (far > reach * reach || far < from * from || ((x + dx + y + dy) & 1)) continue;
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
 * Draws the map: the tiles enlarged, the items on the floors, the cones, the dots, the names, then the buttons, the
 * level and the turn along the top and a line of words along the bottom (who the pointer is on, a button's use, or
 * how the map is worked).
 */
void FirstPersonMapState::draw()
{
	_dirty = false;
	if (_terrainLevel != _level) rebuildTerrain();
	const int width = _view->getWidth(), height = _view->getHeight(), zoom = Zoom, size = cell();
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
				const int side = std::max(2, size / 3);
				_view->drawRect((Sint16)(originX + x * size + (size - side) / 2), (Sint16)(originY + y * size + (size - side) / 2), (Sint16)side, (Sint16)side, primed ? _enemy : _item);
			}
		}
	}

	// The units: where each one's dot is, then the cones under the dots, the dots, and the names over all.
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
		dot.radius = std::clamp(size * 3 / 8, 2, 7) * (tiles > 1 ? 2 : 1);
		if (dot.x < -40 || dot.y < -40 || dot.x > width + 40 || dot.y > height + 40) continue;
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
		drawCone(dot.x, dot.y, dot.radius + 1, degrees, dot.radius + std::max(9, size * 5 / 2), colourOf(dot.unit));
	}
	for (const Dot &dot : _dots)
	{
		drawDisc(dot.x, dot.y, dot.radius, colourOf(dot.unit), dot.unit->getPosition().z != _level);
		// (the selected soldier's dot beats: a white rim every other frame)
		if (dot.unit == selected && (_frame / 2) % 2 == 0)
		{
			for (int step = 0; step < 32; ++step)
			{
				const double turn = step * Pi / 16.0;
				_view->setPixel(dot.x + (int)std::lround((dot.radius + 2) * std::cos(turn)), dot.y + (int)std::lround((dot.radius + 2) * std::sin(turn)), _white);
			}
		}
	}
	_view->unlock();
	// The names: one of ours its number, anyone else its kind's letters; on another level, a mark for above or below.
	// Each beside its dot where it covers no dot and no name written before it (to the right, the left, above, below:
	// the first that is free; the selected soldier's first, then ours, then the others').
	std::vector<SDL_Rect> taken;
	auto free = [&](const SDL_Rect &box) {
		for (const SDL_Rect &other : taken)
			if (box.x < other.x + other.w && other.x < box.x + box.w && box.y < other.y + other.h && other.y < box.y + box.h) return false;
		return true;
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
			const int wide = _words->getTextWidth(), reach = dot.radius + 3;
			const int places[4][2] = { { dot.x + reach, dot.y - WordsHigh / 2 }, { dot.x - reach - wide, dot.y - WordsHigh / 2 },
				{ dot.x - wide / 2, dot.y - reach - WordsHigh }, { dot.x - wide / 2, dot.y + reach } };
			int chosen = 0;
			for (int place = 0; place < 4; ++place)
			{
				const SDL_Rect box = { (Sint16)(places[place][0] - 1), (Sint16)(places[place][1] - 1), (Uint16)(wide + 2), (Uint16)(WordsHigh + 1) };
				if (free(box)) { chosen = place; break; }
			}
			const SDL_Rect box = { (Sint16)(places[chosen][0] - 1), (Sint16)(places[chosen][1] - 1), (Uint16)(wide + 2), (Uint16)(WordsHigh + 1) };
			taken.push_back(box);
			write(name, places[chosen][0], places[chosen][1], dot.unit == selected ? _white : colourOf(dot.unit), true);
		}
	}

	// The buttons (the one under the pointer lit; the names' and the cones' grey when off), the level and the turn.
	const Button *under = buttonAt(_mouseX, _mouseY);
	int right = ButtonGap;
	for (const Button &button : _buttons)
	{
		const bool off = (button.kind == BUTTON_NAMES && !Names) || (button.kind == BUTTON_CONES && !Cones);
		_view->drawRect((Sint16)button.box.x, (Sint16)button.box.y, (Sint16)button.box.w, (Sint16)button.box.h, &button == under ? _white : _edge);
		_view->drawRect((Sint16)(button.box.x + 1), (Sint16)(button.box.y + 1), (Sint16)(button.box.w - 2), (Sint16)(button.box.h - 2), _panel);
		_words->setText(button.words);
		write(button.words, button.box.x + (button.box.w - _words->getTextWidth()) / 2, button.box.y + (ButtonHigh - WordsHigh) / 2 + 1, off ? _grey : _white, false);
		if (button.kind != BUTTON_CLOSE) right = std::max(right, button.box.x + button.box.w);
	}
	// (the level and the turn: after the buttons where the screen has room for them, else under them)
	const std::string level = tr("STR_FPS_TMAP_LEVEL").arg(_level + 1).arg(_save->getMapSizeZ()), turn = tr("STR_FPS_TMAP_TURN").arg(_save->getTurn());
	_words->setText(level + "   " + turn);
	const bool beside = right + 3 * ButtonGap + _words->getTextWidth() + 3 * ButtonGap < _buttons.back().box.x;
	const int wordsX = beside ? right + 3 * ButtonGap : ButtonGap + 1, wordsY = beside ? ButtonGap + (ButtonHigh - WordsHigh) / 2 + 1 : 2 * ButtonGap + ButtonHigh + 1;
	const int levelWide = write(level, wordsX, wordsY, _white, true);
	write(turn, wordsX + levelWide + 3 * ButtonGap, wordsY, _grey, true);

	// The line along the bottom: the button under the pointer, or the unit (its name; one of ours: its time units and
	// health; anyone else: how far from the selected soldier), or how the map is worked.
	std::string line = tr("STR_FPS_TMAP_HINT");
	const Dot *pointed = under ? nullptr : dotAt(_mouseX, _mouseY);
	if (under)
	{
		line = under->tip;
	}
	else if (pointed)
	{
		const BattleUnit *unit = pointed->unit;
		line = unit->getName(_game->getLanguage());
		if (unit->getFaction() == FACTION_PLAYER)
		{
			line += "   " + std::string(tr("STR_FPS_TMAP_SOLDIER").arg(unit->getTimeUnits()).arg(unit->getBaseStats()->tu).arg(unit->getHealth()).arg(unit->getBaseStats()->health));
		}
		else if (selected && !selected->isOut())
		{
			const int dx = unit->getPosition().x - selected->getPosition().x, dy = unit->getPosition().y - selected->getPosition().y;
			line += "   " + std::string(tr("STR_FPS_TMAP_AWAY").arg((int)std::lround(std::sqrt((double)(dx * dx + dy * dy)))));
		}
		if (unit->getPosition().z != _level) line += "   " + std::string(tr("STR_FPS_TMAP_ON_LEVEL").arg(unit->getPosition().z + 1));
	}
	write(line, ButtonGap + 1, height - WordsHigh - ButtonGap, pointed || under ? _white : _grey, true);
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
		const int far = (dot.x - x) * (dot.x - x) + (dot.y - y) * (dot.y - y), reach = dot.radius + DotReach;
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
	case BUTTON_CONES: Cones = !Cones; break;
	case BUTTON_CLOSE: close(); return;
	}
	_dirty = true;
}

/**
 * Looks a level up or down (the game's map: its two buttons, the wheel).
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
	Zoom = std::clamp(Zoom + by, 1, ZoomMost);
	_dirty = true;
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
		if (dot->unit != _save->getSelectedUnit()) _setup.hasViewYaw = false; // (see nextSoldier)
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
 * The pointer moves over the map: a drag scrolls it; else the button or the unit under it is told of along the bottom.
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
		if (std::abs(x - _pressX) > DragStart || std::abs(y - _pressY) > DragStart) _dragged = true;
		if (_dragged)
		{
			_centreX = _pressCentreX - (double)(x - _pressX) / cell();
			_centreY = _pressCentreY - (double)(y - _pressY) / cell();
		}
	}
	_dirty = true;
}

/**
 * The wheel and the keys. The wheel: a level up or down (the game's map's), with Ctrl the zoom. Keys: Escape, the
 * map's key or OK closes; the arrows scroll; the game's level keys, Page Up and Page Down change the level; + and -
 * zoom; Home (or the game's key that centres on the unit) puts the selected soldier in the middle; the game's next
 * and previous unit keys select them.
 * @param action What happened.
 */
void FirstPersonMapState::handle(Action *action)
{
	State::handle(action);
	const SDL_Event *event = action->getDetails();
	if (event->type == SDL_MOUSEBUTTONDOWN && (event->button.button == SDL_BUTTON_WHEELUP || event->button.button == SDL_BUTTON_WHEELDOWN))
	{
		const int by = event->button.button == SDL_BUTTON_WHEELUP ? 1 : -1;
		if (_game->isCtrlPressed()) changeZoom(by);
		else changeLevel(by);
	}
	else if (event->type == SDL_KEYDOWN)
	{
		const SDLKey key = event->key.keysym.sym;
		const double step = std::max(1.0, 24.0 / cell());
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
		_dirty = true;
	}
}

/**
 * The animation's step, eight times a second: every other one the items blink and the selected soldier's dot beats.
 */
void FirstPersonMapState::animate()
{
	_frame = (_frame + 1) % 8;
	if (_frame % 2 == 0) _dirty = true;
}

/**
 * Every frame: the clock, and the map drawn again if anything changed.
 */
void FirstPersonMapState::think()
{
	State::think();
	_clock->think(this, 0);
	if (_dirty) draw();
}

}
