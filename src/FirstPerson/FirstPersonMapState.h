#pragma once
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
 * FirstPersonMapState.h: the tactical map, the battle's map as the 3D view opens it (setting "Tactical map").
 */
#include <functional>
#include <string>
#include <vector>
#include <SDL.h>
#include "../Engine/State.h"
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class BattleUnit;
class Camera;
class InteractiveSurface;
class SavedBattleGame;
class Surface;
class SurfaceSet;
class Text;
class Timer;

/**
 * The tactical map (setting "Tactical map"; the owner: "a better battlescape map for 3d ... full screen with unit
 * numbers by our unit dots and with enemy class abbreviation (like Sec. for Sectoid). Also every dot should have a
 * small 2d cone (green for our, red for enemy, blue for current selected unit) for where it is viewing ... should not
 * have the sprite border of the original but should have small styled button to keep all original map
 * functionality"). It shows what the game's own map screen shows, from the same little pictures of the tiles, the
 * levels up to the one looked at laid over each other, the items on the floors: all over the screen, enlarged by whole
 * steps. Every unit the map may show (ours; the others seen, as on the game's map) is a dot with a cone the way it
 * faces: ours green with its number in the squad, the selected one blue (its cone the way the 3D view looks), an enemy
 * red with its kind's first letters, anyone else yellow. A unit on another level than the one looked at is a ring,
 * with a mark for above or below.
 * All the game's map does is here: the level up and down (buttons, the wheel with Ctrl), a click centres the map
 * there, a drag scrolls it, OK, Escape, the map key or the right button closes it, and the battle's own view is left
 * looking where the map did. More: zoom (the wheel, about the tile under the pointer; buttons, + and -), the map back
 * on the selected soldier, the next soldier, a click on one of ours selects them, the names and the cones switched
 * off and on, and the arrow keys. And: what everyone holds, in a few letters under their dots; the selected soldier's
 * reach, a blue line round every tile they can still walk to with the time units and the energy they have now, by
 * the game's own pathfinding; and a helmet camera in a corner, live: the soldier's the pointer is on, else the
 * selected one's, turned to an enemy the pointer is on. A click on the selected soldier closes the map into their 3D
 * view. On a big screen everything is drawn bigger, by the 3D view's own rule for its overlays; the tiles by the
 * zoom alone.
 * Along the bottom (the owner: "at the bottom is soldier name, health, TU remaining and his/hers weapon - all in
 * colored text as in hud"): the selected soldier's name, health, time units and weapon in hand, each in the colour
 * the 3D view's own panel has it in; with the pointer on a unit, that unit's (anyone not ours: its name, what it
 * holds if the game would name it, how far it is from the selected soldier). The ? button left of OK (the owner: "a
 * help buton that toggles the help as is now at the bottom") shows, over that line, how the map is worked, or what
 * the button under the pointer does; off, there is no help.
 */
class FirstPersonMapState : public State
{
public:
	/// What the 3D view hands the map: the squad in its buttons' order (a unit's number is its place, from 1), the
	/// way the view looks (degrees, 0 east, 90 south; hasViewYaw false: the units' own facing only), and how a
	/// soldier is selected (the view goes to them).
	struct Setup
	{
		std::vector<BattleUnit*> squad;
		double viewYawDegrees = 0.0;
		bool hasViewYaw = false;
		std::function<void(BattleUnit*)> select;
		/// How much bigger everything on the map is drawn on a big screen (the 3D view's own rule for its overlays, 1 or more).
		int uiScale = 1;
		/// Makes a helmet camera's picture of this size and adds it to the map's screen (null: no cameras), and points
		/// it through a soldier's eyes, at a unit if one is given (the 3D view's own cameras, see aimHelmetCamera).
		std::function<Surface*(State*, int, int)> makeCamera;
		std::function<void(Surface*, BattleUnit*, const BattleUnit*)> aimCamera;
		/// The colours the 3D view's own panel writes a soldier's name, health, time units and weapon in (words' colours
		/// in the battle's palette; hasPanelColours false: the map's own), and its words for the weapon a soldier has in
		/// hand, as its panel's line has them: the name, and with true its rounds after it (null: the item's name).
		bool hasPanelColours = false;
		Uint8 nameColour = 0, healthColour = 0, timeColour = 0, weaponColour = 0;
		std::function<std::string(BattleUnit*, bool)> weaponWords;
		/// Shown in a window of another screen (the Live command feed's minimap; a width of 0: all over the screen, a
		/// screen of its own): there it never closes (no OK; the right button does nothing), keeps a zoom of its own,
		/// follows the selected soldier and is drawn again as the battle goes on; its keys are the battle's.
		SDL_Rect window = { 0, 0, 0, 0 };
	};
	/// Creates the tactical map.
	FirstPersonMapState(Camera *camera, SavedBattleGame *save, int maxShade, const Setup &setup);
	/// Cleans up.
	~FirstPersonMapState();
	/// Keys, the wheel.
	void handle(Action *action) override;
	/// The animation's clock.
	void think() override;
	/// Draws the map onto a surface of the screen's size (in a window: the Live command feed's own screen, see
	/// FirstPersonMode::blitFeed).
	void blitOnto(SDL_Surface *target);

private:
	/// A button of the map: what it does, its place, its words.
	enum ButtonKind { BUTTON_LEVEL_UP, BUTTON_LEVEL_DOWN, BUTTON_ZOOM_OUT, BUTTON_ZOOM_IN, BUTTON_CENTRE, BUTTON_NEXT, BUTTON_NAMES, BUTTON_WEAPONS, BUTTON_CONES, BUTTON_REACH, BUTTON_CAMERA, BUTTON_HELP, BUTTON_CLOSE };
	struct Button
	{
		ButtonKind kind;
		SDL_Rect box;
		std::string words, tip;
	};
	/// A unit on the map this frame: where its dot is on the screen and how big.
	struct Dot
	{
		BattleUnit *unit;
		int x, y, radius;
	};
	/// A piece of the line along the bottom: its words and their colour.
	struct Piece
	{
		std::string words;
		Uint8 colour;
	};

	Camera *_camera;
	SavedBattleGame *_save;
	int _maxShade;
	Setup _setup;
	InteractiveSurface *_view = nullptr; // the whole screen: the map, the dots, the buttons
	Surface *_terrain = nullptr;         // the tiles' little pictures for the level looked at, a pixel a pixel
	Text *_words = nullptr;              // the one writer of every word on the map (drawn, then laid on the view)
	SurfaceSet *_tiles = nullptr;        // the game's little pictures (SCANG.DAT)
	Timer *_clock = nullptr;
	std::vector<Button> _buttons;
	std::vector<Dot> _dots;
	int _cornerLeft = 0;                 // where the top right corner's buttons (the help, OK) begin: the first row ends there
	std::string _hint;                   // how the map is worked (the help's words with no button under the pointer)
	int _helpRows = 1;                   // the rows the longest help needs on this screen: kept free over the bottom line while it shows
	int _level = 0, _terrainLevel = -1;
	int _zoom = 1;                         // the zoom shown (the full map's is kept for the next time: see Zoom)
	const BattleUnit *_followed = nullptr; // in a window: the selected soldier it was last centred on
	double _centreX = 0.0, _centreY = 0.0; // the tile in the middle of the screen
	int _frame = 0;                        // the animation's (the items blink, the selected dot beats)
	bool _dirty = true;
	// the pointer
	int _mouseX = -1, _mouseY = -1;
	bool _pressed = false, _dragged = false;
	int _pressX = 0, _pressY = 0;
	double _pressCentreX = 0.0, _pressCentreY = 0.0;
	// the colours, found in the battle's palette
	Uint8 _black = 0, _dark = 0, _panel = 0, _edge = 0, _white = 0, _grey = 0, _ours = 0, _selected = 0, _enemy = 0, _other = 0, _item = 0;
	// the bottom line's: a soldier's name, health, time units and weapon (the 3D view's own panel's, see Setup)
	Uint8 _nameColour = 0, _healthColour = 0, _timeColour = 0, _weaponColour = 0;

	// big screens: everything but the tiles is drawn this many times as big (see Setup::uiScale)
	int _ui = 1;
	// the selected soldier's reach: the tiles they can walk to with the time units and energy they have now (a mark a
	// tile of the map, any level), and whom and what it was worked out for
	std::vector<Uint8> _reach;
	const BattleUnit *_reachUnit = nullptr;
	int _reachTime = -1, _reachEnergy = -1;
	Position _reachFrom;
	// the helmet camera's picture (the 3D view's own kind of view; null: none), whose it shows, and its place
	Surface *_helmet = nullptr;
	BattleUnit *_helmetUnit = nullptr;
	const BattleUnit *_helmetLooksAt = nullptr;

	int cell() const;
	bool embedded() const { return _setup.window.w > 0; }
	int big(int pixels) const { return pixels * _ui; }
	void updateReach();
	void drawReach(int originX, int originY);
	void updateHelmet();
	void placeHelmet();
	std::string weaponOf(const BattleUnit *unit, bool whole = false) const;
	std::vector<Piece> statusOf(BattleUnit *unit, int brief) const;
	std::vector<std::string> wrapped(const std::string &text, int room) const;
	int wideOf(const std::string &text) const;
	void layOut();
	void rebuildTerrain();
	void draw();
	void drawCone(int x, int y, int from, double degrees, int reach, Uint8 colour);
	void drawDisc(int x, int y, int radius, Uint8 colour, bool ring);
	int write(const std::string &text, int x, int y, Uint8 colour, bool backed);
	std::string kindOf(const BattleUnit *unit) const;
	int numberOf(const BattleUnit *unit) const;
	bool shows(const BattleUnit *unit) const;
	const Dot *dotAt(int x, int y) const;
	const Button *buttonAt(int x, int y) const;
	void press(ButtonKind kind);
	void changeLevel(int by);
	void changeZoom(int by);
	void zoomAt(int by, int x, int y);
	void centreOn(const BattleUnit *unit);
	void nextSoldier(int by);
	void close();
	void viewPress(Action *action);
	void viewRelease(Action *action);
	void viewOver(Action *action);
	void animate();
};

}
