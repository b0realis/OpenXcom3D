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
 * FirstPersonScanner.cpp: the motion scanner held up in the 3D view: the game's own screen,
 * turned to the view, with a sweeping ring and its pips.
 */
#include "FirstPersonScanner.h"
#include <algorithm>
#include <cmath>
#include "FirstPersonModeShared.h"
#include "FirstPersonSounds.h"
#include "../Engine/Game.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{

namespace
{
/// Where the round screen lies on the game's scanner art (320x200): its square's top left corner.
const int ArtLeft = 56, ArtTop = 24;
/// How far apart two tiles are on the screen, in pixels, as on the game's scanner.
const int PixelsPerTile = 8;
}

/**
 * The round screen's size across, in pixels (the game's scanner screen's).
 */
int FirstPersonScanner::size()
{
	return 2 * ScannerRadius;
}

/**
 * Prepares the scanner: the round part of the game's scanner screen art (DETBORD2), north up,
 * to turn as the view turns; and the ring's colours, from the game's own blip (its biggest): its
 * brightest colour for the ring's edge, and three shades darker each for its trail, in the same
 * ramp of the palette.
 * @param game The game (its mod's art).
 * @param sounds Our sounds, for the whoop and the pips (null: silent).
 */
FirstPersonScanner::FirstPersonScanner(Game *game, FirstPersonSounds *sounds) : _game(game), _sounds(sounds), _screen(new Surface(size(), size()))
{
	Surface *art = _game->getMod()->getSurface("DETBORD2.PCK");
	_art.assign((size_t)size() * size(), 0);
	art->lock();
	for (int y = 0; y < size(); ++y)
	{
		for (int x = 0; x < size(); ++x) _art[(size_t)y * size() + x] = art->getPixel(ArtLeft + x, ArtTop + y);
	}
	art->unlock();

	Surface *biggestBlip = _game->getMod()->getSurfaceSet("DETBLOB.DAT")->getFrame(5);
	Uint8 brightest = 0;
	biggestBlip->lock();
	for (int y = 0; y < biggestBlip->getHeight(); ++y)
	{
		for (int x = 0; x < biggestBlip->getWidth(); ++x)
		{
			const Uint8 color = biggestBlip->getPixel(x, y);
			if (color && (!brightest || (color & 15) < (brightest & 15))) brightest = color;
		}
	}
	biggestBlip->unlock();
	for (int step = 0; step < 3; ++step)
	{
		_ringColors[step] = (Uint8)((brightest & 0xF0) | std::min(15, (brightest & 15) + 3 * step));
	}
}

/**
 * Frees the round screen it draws on.
 */
FirstPersonScanner::~FirstPersonScanner()
{
	delete _screen;
}

/**
 * Brings the scanner up for a soldier (its use, paid by the caller as the game's): the ring
 * starts from the middle at once, nothing found yet; what the scanner finds now is noted as
 * scanned this turn, as the game's scanner notes it.
 * @param soldier The soldier holding it.
 * @param item The scanner.
 */
void FirstPersonScanner::bringUp(BattleUnit *soldier, BattleItem *item)
{
	_soldier = soldier;
	_item = item;
	_opened = SDL_GetTicks();
	_lastSweep = ~0u;
	_lastRingRadius = 0.0;
	_pinged.clear();
	findBlips(0.0, true);
}

/**
 * Puts the scanner away.
 */
void FirstPersonScanner::putAway()
{
	_soldier = nullptr;
	_item = nullptr;
	_pinged.clear();
}

/**
 * Finds the movement round the soldier now, as the game's scanner does (ScannerView::draw):
 * every unit on the tiles up to 9 across and along from the soldier, on any level, that moved
 * this turn (its motion points), its blip the game's frame for how much (one step for every 5,
 * at most 5). Each is placed on the turned screen (the view's way up), 8 pixels a tile as on the
 * game's; those beyond the round screen's edge (the square's corners) don't show on it.
 * @param bearingRadians The way the view looks (0 north, clockwise).
 * @param noteScanned Whether to note them as scanned this turn, as the game's scanner does (the use).
 * @return The blips.
 */
std::vector<FirstPersonScanner::Blip> FirstPersonScanner::findBlips(double bearingRadians, bool noteScanned) const
{
	std::vector<Blip> blips;
	SavedBattleGame *save = _game->getSavedGame()->getSavedBattle();
	const double cosine = std::cos(bearingRadians), sine = std::sin(bearingRadians);
	for (int x = -9; x < 10; ++x)
	{
		for (int y = -9; y < 10; ++y)
		{
			for (int z = 0; z < save->getMapSizeZ(); ++z)
			{
				Tile *tile = save->getTile(Position(x, y, z) + Position(_soldier->getPosition().x, _soldier->getPosition().y, 0));
				if (!tile || !tile->getUnit() || !tile->getUnit()->getMotionPoints()) continue;
				const int frame = tile->getUnit()->getMotionPoints() / 5;
				if (frame < 0) continue;
				if (noteScanned) tile->getUnit()->setScannedTurn(save->getTurn());
				// On the turned screen: the map's east and south turned by minus the view's bearing.
				Blip blip;
				blip.x = PixelsPerTile * (x * cosine + y * sine);
				blip.y = PixelsPerTile * (-x * sine + y * cosine);
				blip.radius = std::hypot(blip.x, blip.y);
				blip.angle = std::atan2(blip.x, -blip.y) * 180.0 / Pi;
				blip.frame = std::min(frame, 5);
				blip.unit = tile->getUnit();
				blip.tileIndex = save->getTileIndex(tile->getPosition());
				if (blip.radius <= ScannerRadius - 4) blips.push_back(blip);
			}
		}
	}
	return blips;
}

/**
 * Draws the round screen for now onto the panel: the game's scanner art turned so the view's
 * way is up (each pixel of the round screen takes the art's pixel that far round the other way;
 * outside it, nothing), a rim round it; the ring sweeping out from the middle in ScannerSweepMs,
 * a new sweep every ScannerPeriodMs with the whoop, drawn as a dotted circle in the blips' bright
 * colour with two fading circles behind it; each movement the ring has reached (a pip as it
 * does: higher the nearer, from its side, at most one every ScannerPipSpacingMs), as the game
 * draws its blip (its frame, flickering between two as the game's do), fading over
 * ScannerBlipFadeMs; and in the middle the game's arrow pointing up, the way the view looks. All
 * drawn at the art's own size, then scaled onto the panel (pixel for pixel, the nearest).
 * @param panel The surface to draw on (square).
 * @param bearingDegrees The way the view looks (0 north, clockwise).
 * @param rimColor The rim's colour.
 */
void FirstPersonScanner::draw(Surface *panel, double bearingDegrees, Uint8 rimColor)
{
	const Uint32 now = SDL_GetTicks(), sinceOpened = now - _opened;
	const double bearingRadians = bearingDegrees * Pi / 180.0;
	const int middle = ScannerRadius;

	// The ring: where it is, and a new sweep's whoop.
	const Uint32 sweep = sinceOpened / ScannerPeriodMs, intoSweep = sinceOpened % ScannerPeriodMs;
	const double ringRadius = intoSweep < ScannerSweepMs ? (double)intoSweep / ScannerSweepMs * ScannerRadius : -1.0;
	if (sweep != _lastSweep)
	{
		_lastSweep = sweep;
		_lastRingRadius = 0.0;
		if (_sounds) _sounds->play(FirstPersonSounds::SCAN_PULSE, 0, 0.7f);
	}

	// The movement round the soldier now, and the blips the ring passes this frame: lit, with a pip.
	const std::vector<Blip> blips = findBlips(bearingRadians, false);
	if (ringRadius >= 0.0)
	{
		for (const Blip &blip : blips)
		{
			if (blip.radius > _lastRingRadius && blip.radius <= ringRadius)
			{
				_pinged[{ blip.unit, blip.tileIndex }] = now;
				if (_sounds && now - _lastPip >= ScannerPipSpacingMs)
				{
					_lastPip = now;
					const FirstPersonSounds::Id pip = blip.radius < 3 * PixelsPerTile ? FirstPersonSounds::SCAN_BLIP_NEAR
						: blip.radius < 6 * PixelsPerTile ? FirstPersonSounds::SCAN_BLIP_MID : FirstPersonSounds::SCAN_BLIP_FAR;
					_sounds->play(pip, (int)std::lround(blip.angle), 0.9f);
				}
			}
		}
		_lastRingRadius = ringRadius;
	}

	// The art, turned, and its rim.
	_screen->clear();
	_screen->lock();
	const double cosine = std::cos(bearingRadians), sine = std::sin(bearingRadians);
	for (int y = 0; y < size(); ++y)
	{
		for (int x = 0; x < size(); ++x)
		{
			const double dx = x + 0.5 - middle, dy = y + 0.5 - middle, distanceSq = dx * dx + dy * dy;
			if (distanceSq > ScannerRadius * ScannerRadius) continue;
			if (distanceSq > (ScannerRadius - 2) * (ScannerRadius - 2))
			{
				_screen->setPixel(x, y, rimColor);
				continue;
			}
			const int fromX = (int)std::floor(middle + dx * cosine - dy * sine), fromY = (int)std::floor(middle + dx * sine + dy * cosine);
			if (fromX < 0 || fromY < 0 || fromX >= size() || fromY >= size()) continue;
			_screen->setPixel(x, y, _art[(size_t)fromY * size() + fromX]);
		}
	}
	// The ring and its trail, 2 and 4 pixels behind it: a dot every pixel or so round it.
	for (int trail = 0; trail < 3 && ringRadius >= 0.0; ++trail)
	{
		const double radius = ringRadius - 2.0 * trail;
		if (radius < 1.0) continue;
		const int dots = std::max(8, (int)(2.0 * Pi * radius));
		for (int dot = 0; dot < dots; ++dot)
		{
			const double angle = 2.0 * Pi * dot / dots;
			const int x = middle + (int)std::lround(radius * std::sin(angle)), y = middle - (int)std::lround(radius * std::cos(angle));
			if (x >= 0 && y >= 0 && x < size() && y < size()) _screen->setPixel(x, y, _ringColors[trail]);
		}
	}
	_screen->unlock();

	// The blips the ring has found, fading (up to 10 shades darker) till the next sweep.
	SurfaceSet *blipFrames = _game->getMod()->getSurfaceSet("DETBLOB.DAT");
	const int flicker = (int)(sinceOpened / 125 % 2);
	for (const Blip &blip : blips)
	{
		auto pinged = _pinged.find({ blip.unit, blip.tileIndex });
		if (pinged == _pinged.end() || now - pinged->second > ScannerBlipFadeMs) continue;
		const int shade = (int)((now - pinged->second) * 10 / ScannerBlipFadeMs);
		Surface *frame = blipFrames->getFrame(blip.frame + flicker);
		frame->blitNShade(_screen, middle + (int)std::lround(blip.x) - frame->getWidth() / 2, middle + (int)std::lround(blip.y) - frame->getHeight() / 2, shade);
	}
	// The soldier's arrow, pointing up (the game's frame for north).
	Surface *arrow = blipFrames->getFrame(7);
	arrow->blitNShade(_screen, middle - arrow->getWidth() / 2, middle - arrow->getHeight() / 2, 0);

	// Onto the panel, scaled: each of its pixels the nearest of the round screen's.
	const int panelSize = panel->getWidth();
	panel->lock();
	_screen->lock();
	for (int y = 0; y < panelSize; ++y)
	{
		for (int x = 0; x < panelSize; ++x) panel->setPixel(x, y, _screen->getPixel(x * size() / panelSize, y * size() / panelSize));
	}
	_screen->unlock();
	panel->unlock();
}

}
