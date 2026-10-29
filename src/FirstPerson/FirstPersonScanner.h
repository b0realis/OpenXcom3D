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
 * FirstPersonScanner.h: the motion scanner held up in the 3D view.
 */
#include <map>
#include <utility>
#include <vector>
#include <SDL.h>

namespace OpenXcom
{

class BattleItem;
class BattleUnit;
class FirstPersonSounds;
class Game;
class Surface;

/**
 * The motion scanner held up in the 3D view (a player's report, GitHub issue #39; the owner:
 * "keep it simple ... just orient the screen to where player is looking in 3d and make some
 * retro animation like concentric circles from the center and bumping into signals that
 * refresh. And some cool scary beep upon detection (like alien movie)", and "the 3d scanner
 * should not overtake whole screen. It should only be a part of 3d screen"; on the isometric map
 * the game's own scanner, as ever). Using it (its time units, as the game's) brings up a round
 * screen at the bottom middle of the view, the game's scanner screen (DETBORD2's round part),
 * turned so the way the view looks is up as the view turns (its N, E, S and W go round with
 * it). It shows what the game's scanner finds (the units that moved this turn, up to 9 tiles
 * across and along, a bigger blip the more they moved), placed from where the soldier stands now.
 * A ring sweeps out from the middle every few seconds with a whoop; a blip shows when the ring
 * reaches it, with a pip (higher the nearer, from its side), and fades until the next sweep finds
 * it again. It stays up while the scanner is the item in hand (see FirstPersonMode::updateScanner).
 */
class FirstPersonScanner
{
public:
	/// Prepares the scanner: the game's art and blips, and our sounds (null: silent).
	FirstPersonScanner(Game *game, FirstPersonSounds *sounds);
	/// Frees its own screen.
	~FirstPersonScanner();
	/// Brings the scanner up for a soldier (its use, paid): what it finds now is noted as scanned this turn.
	void bringUp(BattleUnit *soldier, BattleItem *item);
	/// Puts the scanner away.
	void putAway();
	/// Is the scanner up?
	bool isUp() const { return _soldier != nullptr; }
	/// The soldier holding it up, or null.
	const BattleUnit *soldier() const { return _soldier; }
	/// The scanner held up, or null.
	const BattleItem *item() const { return _item; }
	/// Draws the round screen for now onto the panel (scaled to its size), the view looking the way of bearingDegrees.
	void draw(Surface *panel, double bearingDegrees, Uint8 rimColor);
	/// The round screen's size across, in pixels of the game's scanner art.
	static int size();
private:
	/// One movement shown: where it is on the screen (pixels from its middle), how far out
	/// (pixels), which way it lies from the view (degrees, 0 ahead, 90 right), and its blip (the
	/// game's frame for how much the unit moved).
	struct Blip { double x, y, radius, angle; int frame; const BattleUnit *unit; int tileIndex; };
	Game *_game;
	FirstPersonSounds *_sounds;                 // Our sounds (the whoop and the pips), or null.
	std::vector<Uint8> _art;                    // The game's scanner screen (DETBORD2's round part, a square as wide), north up.
	Surface *_screen;                           // The round screen drawn at the art's own size, then scaled onto the panel.
	BattleUnit *_soldier = nullptr;             // The soldier holding it up (null: put away).
	BattleItem *_item = nullptr;                // The scanner held up.
	Uint32 _opened = 0;                         // When it came up (ms), the sweeps' clock.
	Uint32 _lastSweep = ~0u;                    // Which sweep the ring is on (counted from the opening).
	Uint32 _lastPip = 0;                        // When the last pip played (ms).
	double _lastRingRadius = 0.0;               // How far out the ring was last frame (pixels).
	Uint8 _ringColors[3] = {};                  // The ring's colours: its bright edge, then its fading trail.
	std::map<std::pair<const BattleUnit*, int>, Uint32> _pinged; // When the ring last reached each unit's tile (by the tile's index, ms).
	/// Finds the movement round the soldier now, as the game's scanner does, placed on the turned screen.
	std::vector<Blip> findBlips(double bearingRadians, bool noteScanned) const;
};

}
