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
 * All the game's map does is here: the level up and down (buttons, the wheel), a click centres the map there, a drag
 * scrolls it, OK, Escape, the map key or the right button closes it, and the battle's own view is left looking where
 * the map did. More: zoom (buttons, + and -, the wheel with Ctrl), the map back on the selected soldier, the next
 * soldier, a click on one of ours selects them, the names and the cones switched off and on, the arrow keys, and a
 * line saying who the pointer is on and how far they are from the selected soldier.
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
	};
	/// Creates the tactical map.
	FirstPersonMapState(Camera *camera, SavedBattleGame *save, int maxShade, const Setup &setup);
	/// Cleans up.
	~FirstPersonMapState();
	/// Keys, the wheel.
	void handle(Action *action) override;
	/// The animation's clock.
	void think() override;

private:
	/// A button of the map: what it does, its place, its words.
	enum ButtonKind { BUTTON_LEVEL_UP, BUTTON_LEVEL_DOWN, BUTTON_ZOOM_OUT, BUTTON_ZOOM_IN, BUTTON_CENTRE, BUTTON_NEXT, BUTTON_NAMES, BUTTON_CONES, BUTTON_CLOSE };
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
	int _level = 0, _terrainLevel = -1;
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

	int cell() const;
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
	void centreOn(const BattleUnit *unit);
	void nextSoldier(int by);
	void close();
	void viewPress(Action *action);
	void viewRelease(Action *action);
	void viewOver(Action *action);
	void animate();
};

}
