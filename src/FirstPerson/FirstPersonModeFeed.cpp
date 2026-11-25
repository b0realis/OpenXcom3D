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
 * FirstPersonModeFeed.cpp: the Live command feed (setting "Live command feed", experimental): the isometric map as a
 * command post's screens. The owner: "the iso mode is replaced with realistic combat dashboard" (police tactical units'
 * helmet cameras streaming to a command post): along the top a row of the soldiers' helmet cameras (to watch, with
 * arrows to scroll through the squad), below the map as a drone's live feed (black and white with static, or night
 * vision; each soldier's number and name over them, a box round each) and the tactical map with all its buttons
 * beside it. Each window has a button to show it all over the screen. The drone's feed is the game's own map (its
 * clicks, its scrolling, its panel under it) shown in a window; the minimap is the tactical map in a window.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <SDL_mixer.h>
#include "FirstPersonApi.h"
#include "FirstPersonMapState.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/Options.h"
#include "../Engine/Sound.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

namespace
{

/// The windows' borders are this thick, their title lines this high, the gaps between the helmet cameras this wide
/// (all times the screen's magnification); a line of words is this high, a title's button this far inside its line.
const int FeedBorder = 1, FeedTitleH = 11, FeedGap = 2, FeedWordsH = 9, FeedButtonInset = 1, FeedButtonPad = 3;
/// The arrows at the ends of the row of cameras are this wide.
const int FeedArrowW = 12;
/// A helmet camera's picture is at least this wide for its height (4:3), and at the most this (16:9).
const double FeedPictureNarrowest = 4.0 / 3.0, FeedPictureWidest = 16.0 / 9.0;
/// The drone's window takes this share of the screen's width, the minimap the rest, if that is at least FeedMapLeast
/// wide (else the drone has it all).
const double FeedDroneShare = 0.6;
const int FeedMapLeast = 120;
/// The drone's picture: a speck of snow in every this many pixels; a band this many rows high, a little brighter,
/// rolls down it this many milliseconds a row; every this many milliseconds the picture tears for this long.
const int FeedSnowEvery = 70, FeedBandRows = 5, FeedBandRowMs = 30, FeedTearEvery = 3700, FeedTearFor = 140;
/// The marks over a soldier on the drone's picture: the corners of the box this long.
const int FeedCornerLength = 4;

/// An SDL rectangle from ints (SDL 1.2's are 16-bit).
SDL_Rect makeRect(int x, int y, int width, int height)
{
	SDL_Rect rect;
	rect.x = (decltype(rect.x))x;
	rect.y = (decltype(rect.y))y;
	rect.w = (decltype(rect.w))std::max(0, width);
	rect.h = (decltype(rect.h))std::max(0, height);
	return rect;
}

/// Is a point in a rectangle?
bool inside(const SDL_Rect &rect, int x, int y)
{
	return rect.w > 0 && x >= rect.x && y >= rect.y && x < rect.x + (int)rect.w && y < rect.y + (int)rect.h;
}

/// Are two rectangles the same?
bool sameRect(const SDL_Rect &a, const SDL_Rect &b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/// A filled rectangle on the screen (inside its clip).
void fill(SDL_Surface *screen, int x, int y, int width, int height, Uint8 colour)
{
	if (width <= 0 || height <= 0) return;
	SDL_Rect rect = makeRect(x, y, width, height);
	SDL_FillRect(screen, &rect, colour);
}

/// A rectangle's outline, this thick.
void outline(SDL_Surface *screen, const SDL_Rect &rect, int thick, Uint8 colour)
{
	fill(screen, rect.x, rect.y, rect.w, thick, colour);
	fill(screen, rect.x, rect.y + rect.h - thick, rect.w, thick, colour);
	fill(screen, rect.x, rect.y, thick, rect.h, colour);
	fill(screen, rect.x + rect.w - thick, rect.y, thick, rect.h, colour);
}

}

/**
 * The Live command feed's state (see FirstPersonMode::updateFeed): where its windows are, the helmet cameras' views,
 * the minimap, the drone's switches, what of the battle's screen it changed, and its colours.
 */
struct CommandFeed
{
	/// Which window is all over the screen.
	enum Maximised { MAX_NONE, MAX_CAMERA, MAX_DRONE };
	/// A button in a window's title line (or an arrow of the row): what it does, its place, its words (none: a drawn
	/// sign), for which camera, and whether it shows lit (a switch that is on).
	enum ButtonKind { FB_NIGHT, FB_NAMES, FB_BOXES, FB_DRONE_MAX, FB_MAP_MAX, FB_CAMERA_MAX, FB_LEFT, FB_RIGHT, FB_BIG_PREV, FB_BIG_NEXT, FB_BIG_RESTORE };
	struct Button
	{
		ButtonKind kind;
		SDL_Rect box;
		std::string words;
		int camera;
		bool lit;
	};
	/// A helmet camera's window in the row: its view, whom it shows (and showed when last drawn), its frame and picture.
	struct Window
	{
		std::unique_ptr<FirstPersonView> view;
		BattleUnit *unit = nullptr;
		const BattleUnit *drawnFor = nullptr;
		int number = 0;
		SDL_Rect frame{}, picture{};
	};

	// The layout and what it was made for (the screen's size, the top bar's foot, the squad's count).
	int width = 0, height = 0, top = -1, soldiers = -1, ui = 1;
	SDL_Rect row{}, drone{}, dronePicture{}, map{}, mapPicture{}, bigFrame{}, bigPicture{};
	std::vector<Window> windows;
	std::vector<Button> buttons;
	int first = 0, nextDrawn = 0, frames = 0;
	const BattleUnit *selected = nullptr, *alerted = nullptr;
	// A window all over the screen: which, and for a helmet camera its own view and soldier.
	Maximised maximised = MAX_NONE;
	std::unique_ptr<FirstPersonView> big;
	BattleUnit *bigUnit = nullptr;
	// The minimap: the tactical map in its window.
	std::unique_ptr<FirstPersonMapState> minimap;
	SDL_Rect minimapWindow{};
	// The drone's switches: night vision (else black and white), the soldiers' names, the boxes round them.
	bool nightVision = false, names = true, boxes = true;
	// What it changed on the battle's screen: the game's panel moved this far (under the drone), the map's camera in
	// this window (see Camera::setViewWindow); shown this frame.
	int shift = 0;
	SDL_Rect cameraWindow{};
	bool on = false;
	// The mouse: a press on the drone's picture, the map's until it comes up; over the drone's picture last.
	bool pressedOnDrone = false, overDrone = false;
	// The colours, from the battle's palette (made again if it changes): the drone's black and white and night vision
	// (plain, a scan line's darker and the rolling band's brighter), the snow, and the windows' own.
	SDL_Color palette[256];
	bool paletteKnown = false;
	Uint8 grey[3][256], green[3][256], snow[8];
	Uint8 black = 0, frameColour = 0, frameLit = 0, titleBack = 0, wordsColour = 0, dimColour = 0, liveColour = 0, hudColour = 0, pickedColour = 0;
	std::unique_ptr<Text> writer;
	Uint32 noise = 0x9E3779B9u;
	/// The snow's and the tears' dice.
	Uint32 random()
	{
		noise ^= noise << 13;
		noise ^= noise >> 17;
		noise ^= noise << 5;
		return noise;
	}
	/// The drone's frame and picture now: its window, or all over the screen.
	const SDL_Rect &droneFrame() const { return maximised == MAX_DRONE ? bigFrame : drone; }
	const SDL_Rect &dronePictureNow() const { return maximised == MAX_DRONE ? bigPicture : dronePicture; }
};

void CommandFeedDeleter::operator()(CommandFeed *feed) const
{
	delete feed;
}

/**
 * Does the Live command feed take the isometric map's place now? With its setting on, whenever the map shows (not
 * the 3D view; never in VR mode, whose headset shows the 3D view).
 * @return True if so.
 */
bool FirstPersonMode::feedWanted() const
{
	return Options::fpsCommandFeed && !isActive() && !vrOn();
}

/**
 * Is the Live command feed on the screen (this frame)?
 * @return True if so.
 */
bool FirstPersonMode::feedShown() const
{
	return _feed && _feed->on;
}

/**
 * Each frame (see think): the Live command feed, when wanted, laid out for the screen and the squad, the battle's
 * screen put right for it (the game's panel under the drone's window, the map's camera in it), the selected soldier
 * kept in the row of cameras, one helmet camera drawn again (they take turns; the one all over the screen every
 * frame), and the minimap's frame. Not wanted: the battle's screen as it was.
 */
void FirstPersonMode::updateFeed()
{
	if (!feedWanted())
	{
		if (feedShown()) leaveFeed();
		return;
	}
	if (!_feed)
	{
		_feed.reset(new CommandFeed());
	}
	CommandFeed &feed = *_feed;
	feed.on = true;
	feedColours();
	layOutFeed();

	// The row: the squad from its first soldier shown; brought into it, the selected soldier when another is selected,
	// and a soldier whose camera the aliens' turn pops up (see alienTurnMapCameras: hit, shot at, firing back,
	// watching an alien; the card that pops up on the map isn't shown with the feed).
	const std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
	const int count = (int)squad.size(), shown = (int)feed.windows.size();
	auto bringIn = [&](const BattleUnit *unit) {
		for (int place = 0; place < count; ++place)
		{
			if (squad[place].first != unit) continue;
			if (place < feed.first) feed.first = place;
			if (place >= feed.first + shown) feed.first = place - shown + 1;
		}
	};
	const BattleUnit *selected = _save->getSelectedUnit();
	if (selected != feed.selected)
	{
		feed.selected = selected;
		bringIn(selected);
	}
	const BattleUnit *alerted = _alertUnit && SDL_GetTicks() < _alertUntil ? _alertUnit : nullptr;
	if (alerted != feed.alerted)
	{
		feed.alerted = alerted;
		bringIn(alerted);
	}
	feed.first = std::clamp(feed.first, 0, std::max(0, count - shown));
	for (int window = 0; window < shown; ++window)
	{
		const int place = feed.first + window;
		feed.windows[window].unit = place < count ? squad[place].first : nullptr;
		feed.windows[window].number = place < count ? squad[place].second : 0;
	}
	// (the one all over the screen: back to the row if their soldier is gone)
	if (feed.maximised == CommandFeed::MAX_CAMERA && (!feed.bigUnit || feed.bigUnit->isOut() || !feed.bigUnit->getTile()))
	{
		feed.maximised = CommandFeed::MAX_NONE;
		feed.big.reset();
		feed.bigUnit = nullptr;
	}
	feedButtons();
	placeFeedScreen();

	// One camera drawn again: the one all over the screen; else a window whose soldier changed first, then every other
	// frame the popped-up soldier's (looking at what it popped up for), then in turn.
	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		if (!feed.big || feed.big->getWidth() != feed.bigPicture.w || feed.big->getHeight() != feed.bigPicture.h)
		{
			feed.big.reset(makeFeedCamera(feed.bigPicture.w, feed.bigPicture.h));
		}
		aimHelmetCamera(feed.big.get(), feed.bigUnit, feed.bigUnit == alerted ? _alertLookAt : nullptr);
	}
	else if (shown > 0)
	{
		int drawn = -1;
		for (int window = 0; window < shown && drawn < 0; ++window)
		{
			if (feed.windows[window].unit != feed.windows[window].drawnFor) drawn = window;
		}
		for (int window = 0; window < shown && drawn < 0 && alerted && ++feed.frames % 2 == 0; ++window)
		{
			if (feed.windows[window].unit == alerted) drawn = window;
		}
		if (drawn < 0) drawn = feed.nextDrawn++ % shown;
		CommandFeed::Window &window = feed.windows[drawn];
		if (window.unit)
		{
			aimHelmetCamera(window.view.get(), window.unit, window.unit == alerted ? _alertLookAt : nullptr);
		}
		window.drawnFor = window.unit;
	}

	// The minimap's frame (its clock, drawn again as the battle goes on).
	if (feed.minimap && feed.maximised == CommandFeed::MAX_NONE)
	{
		feed.minimap->think();
	}
}

/**
 * The Live command feed leaves the screen (the 3D view shows, or its setting went off): the game's panel back in its
 * place, the map's camera on the whole screen again around what it looked at, and the feed's views let go (made again
 * when it comes back).
 */
void FirstPersonMode::leaveFeed()
{
	CommandFeed &feed = *_feed;
	moveGamePanel(-feed.shift);
	feed.shift = 0;
	if (feed.cameraWindow.w > 0)
	{
		Camera *camera = _state->_map->getCamera();
		const Position centre = camera->getCenterPosition();
		camera->setViewWindow(0, 0, 0, 0);
		camera->centerOnPosition(centre, false);
		feed.cameraWindow = SDL_Rect{};
	}
	_state->_map->getCamera()->stopMouseScrolling();
	feed.windows.clear();
	feed.big.reset();
	feed.bigUnit = nullptr;
	feed.minimap.reset();
	feed.maximised = CommandFeed::MAX_NONE;
	feed.width = 0; // (laid out again when it comes back)
	feed.pressedOnDrone = feed.overDrone = false;
	feed.on = false;
}

/**
 * The squad as the row of cameras shows it: the soldiers who are up and on the map, in the squad buttons' order, each
 * with their number there (see squadList).
 * @return The soldiers and their numbers.
 */
std::vector<std::pair<BattleUnit*, int>> FirstPersonMode::feedSquad() const
{
	std::vector<std::pair<BattleUnit*, int>> squad;
	const std::vector<BattleUnit*> all = squadList();
	for (size_t place = 0; place < all.size(); ++place)
	{
		BattleUnit *unit = all[place];
		if (unit->getFaction() == FACTION_PLAYER && !unit->isOut() && unit->getTile())
		{
			squad.emplace_back(unit, (int)place + 1);
		}
	}
	return squad;
}

/**
 * A helmet camera's view for the feed: the map camera's kind (see makeMapCamera), sharing the 3D view's models,
 * reaching at most 30 tiles, without particles, with the helmet cameras' static and bad signal.
 * @param width The picture's width, in the game's screen pixels.
 * @param height Its height.
 * @return The view (the caller's).
 */
FirstPersonView *FirstPersonMode::makeFeedCamera(int width, int height)
{
	const Mod *mod = _game->getMod();
	FirstPersonView *camera = new FirstPersonView(_game, std::max(8, width), std::max(8, height), 0, 0);
	camera->setPalette(_state->getPalette());
	camera->shareCache(_view); // (first: then the battle doesn't build models of its own)
	camera->useVoxelUnits(_voxelUnits.get());
	camera->setSavedBattleGame(_save);
	camera->setMap(_state->_map);
	camera->setMaxDistance(std::min(30, mod->getFirstPerson().getMaxViewDistance()));
	camera->setParticles(false);
	camera->setStaticSpells(true);
	camera->setSignalGlitches(mod->getFirstPerson().getSignalLevel(_save->getMissionType()));
	return camera;
}

/**
 * The 3D view was made again (see makeView): the feed's cameras share its models now.
 * @param owner The new view.
 */
void FirstPersonMode::feedShareCache(FirstPersonView *owner)
{
	if (!_feed) return;
	for (CommandFeed::Window &window : _feed->windows)
	{
		if (window.view) window.view->shareCache(owner);
	}
	if (_feed->big) _feed->big->shareCache(owner);
}

/**
 * The feed's colours, from the battle's palette, made again when it changes: the drone's black and white and night
 * vision for each colour (as it is, a scan line's darker, the rolling band's brighter), the snow's greys, and the
 * windows' own (the nearest the palette has; words kept off the dark end of their run, which the letters shade down).
 */
void FirstPersonMode::feedColours()
{
	CommandFeed &feed = *_feed;
	const SDL_Color *palette = _state->getPalette();
	if (feed.paletteKnown && std::memcmp(feed.palette, palette, sizeof(feed.palette)) == 0)
	{
		return;
	}
	std::memcpy(feed.palette, palette, sizeof(feed.palette));
	feed.paletteKnown = true;
	const double shades[3] = { 1.0, 0.78, 1.22 };
	for (int colour = 0; colour < 256; ++colour)
	{
		const double light = 0.30 * palette[colour].r + 0.59 * palette[colour].g + 0.11 * palette[colour].b;
		for (int shade = 0; shade < 3; ++shade)
		{
			// (a little more contrast than the picture had: a camera's)
			const int level = std::clamp((int)std::lround((light - 128.0) * 1.15 * shades[shade] + 128.0 * shades[shade]), 0, 255);
			feed.grey[shade][colour] = nearestColor(palette, level, level, level);
			feed.green[shade][colour] = nearestColor(palette, level / 5, level, level / 5);
		}
	}
	for (int speck = 0; speck < 8; ++speck)
	{
		const int level = 60 + speck * 25;
		feed.snow[speck] = nearestColor(palette, level, level, level);
	}
	auto bright = [&](int red, int green, int blue) {
		const Uint8 colour = nearestColor(palette, red, green, blue);
		return (Uint8)((colour & 0xF0) | std::min(colour & 0x0F, 9));
	};
	feed.black = nearestColor(palette, 0, 0, 0);
	feed.frameColour = nearestColor(palette, 70, 80, 100);
	feed.frameLit = nearestColor(palette, 90, 150, 255);
	feed.titleBack = nearestColor(palette, 18, 22, 32);
	feed.wordsColour = bright(225, 225, 225);
	feed.dimColour = bright(130, 135, 150);
	feed.liveColour = nearestColor(palette, 230, 30, 30);
	feed.hudColour = bright(90, 235, 90);
	feed.pickedColour = bright(250, 215, 60);
	if (!feed.writer)
	{
		feed.writer.reset(new Text(Options::baseXResolution, FeedWordsH, 0, 0));
		feed.writer->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
		feed.writer->setSmall();
	}
	feed.writer->setPalette(palette);
}

/**
 * Lays the feed out for the screen (in the game's screen pixels), again only when the screen, the top bar's foot or
 * the squad's count changed: the row of helmet cameras from under the top bar to the screen's middle, as many windows
 * as fit at 4:3 (none wider than 16:9), centred, with arrows at its ends when the squad has more soldiers than fit; the
 * drone's window under it at the left, the minimap's at the right (if there is room), and the window all over the
 * screen. The views and the minimap are made for their windows.
 */
void FirstPersonMode::layOutFeed()
{
	CommandFeed &feed = *_feed;
	const int width = Options::baseXResolution, height = Options::baseYResolution;
	const int top = hudHidden() ? 0 : toScreenY(BarY + BarH) + 1;
	const int soldiers = (int)feedSquad().size();
	if (width == feed.width && height == feed.height && top == feed.top && soldiers == feed.soldiers)
	{
		return;
	}
	feed.width = width;
	feed.height = height;
	feed.top = top;
	feed.soldiers = soldiers;
	feed.ui = uiScaleFor(width, height);
	const int ui = feed.ui, border = FeedBorder * ui, titleH = FeedTitleH * ui, gap = FeedGap * ui, middle = height / 2;

	// The row of helmet cameras: as many as fit at 4:3, with arrows when the squad has more.
	feed.row = makeRect(0, top, width, middle - gap - top);
	const int pictureH = std::max(8, (int)feed.row.h - titleH - 2 * border);
	const int narrowest = (int)(pictureH * FeedPictureNarrowest) + 2 * border, widest = (int)(pictureH * FeedPictureWidest) + 2 * border;
	int arrows = 0;
	int shown = std::min(soldiers, std::max(1, (width + gap) / (narrowest + gap)));
	if (soldiers > shown)
	{
		arrows = FeedArrowW * ui + gap;
		shown = std::min(soldiers, std::max(1, (width - 2 * arrows + gap) / (narrowest + gap)));
	}
	const int room = width - 2 * arrows;
	const int windowW = shown > 0 ? std::min(widest, (room - (shown - 1) * gap) / shown) : 0;
	int x = arrows + (room - (shown * windowW + std::max(0, shown - 1) * gap)) / 2;
	feed.windows.resize(shown);
	for (CommandFeed::Window &window : feed.windows)
	{
		window.frame = makeRect(x, top, windowW, feed.row.h);
		window.picture = makeRect(x + border, top + border + titleH, windowW - 2 * border, feed.row.h - 2 * border - titleH);
		if (!window.view || window.view->getWidth() != window.picture.w || window.view->getHeight() != window.picture.h)
		{
			window.view.reset(makeFeedCamera(window.picture.w, window.picture.h));
			window.drawnFor = nullptr;
		}
		x += windowW + gap;
	}

	// The drone's window and the minimap's: the drone's at least as wide as the game's panel under it.
	const int least = _state->_map->getIconWidth() + 2 * border;
	int droneW = std::max(least, (int)(width * FeedDroneShare));
	if (width - droneW < FeedMapLeast * ui) droneW = width;
	feed.drone = makeRect(0, middle, droneW, height - middle);
	feed.dronePicture = makeRect(border, middle + border + titleH, droneW - 2 * border, height - middle - border - titleH);
	feed.map = droneW < width ? makeRect(droneW, middle, width - droneW, height - middle) : SDL_Rect{};
	feed.mapPicture = droneW < width ? makeRect(droneW + border, middle + border + titleH, width - droneW - 2 * border, height - middle - 2 * border - titleH) : SDL_Rect{};
	feed.bigFrame = makeRect(0, top, width, height - top);
	feed.bigPicture = makeRect(border, top + border + titleH, width - 2 * border, height - top - border - titleH);

	// The minimap: the tactical map in its window, without its helmet camera (made again only for another window: a
	// soldier falling keeps its zoom and level).
	if (feed.minimap && sameRect(feed.minimapWindow, feed.mapPicture))
	{
		return;
	}
	feed.minimap.reset();
	feed.minimapWindow = feed.mapPicture;
	if (feed.mapPicture.w > 0)
	{
		FirstPersonMapState::Setup setup = tacticalMapSetup(false);
		setup.window = feed.mapPicture;
		feed.minimap.reset(new FirstPersonMapState(_state->_map->getCamera(), _save, _state->_map->reShadeMinimap(7), setup));
	}
}

/**
 * The feed's buttons for this frame, in the windows' title lines (filled from the right): each helmet camera's to show
 * it all over the screen; the drone's night vision, names, boxes and all over the screen; the minimap's all over the
 * screen; the row's arrows; and with a camera all over the screen, the soldier before, the next, and back.
 */
void FirstPersonMode::feedButtons()
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, border = FeedBorder * ui, titleH = FeedTitleH * ui, inset = FeedButtonInset * ui, side = titleH - 2 * inset;
	feed.buttons.clear();
	// (a button at the right of a title line, left of the last one: its words' width, or a square for a sign)
	auto add = [&](CommandFeed::ButtonKind kind, const SDL_Rect &frame, int &right, const std::string &words, int camera, bool lit) {
		int wide = side;
		if (!words.empty())
		{
			feed.writer->setText(words);
			wide = std::max(side, feed.writer->getTextWidth() * ui + 2 * FeedButtonPad * ui);
		}
		right -= wide + inset;
		feed.buttons.push_back({ kind, makeRect(right, frame.y + border + inset, wide, side), words, camera, lit });
	};
	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		int right = feed.bigFrame.x + feed.bigFrame.w - border;
		add(CommandFeed::FB_BIG_RESTORE, feed.bigFrame, right, "", -1, true);
		add(CommandFeed::FB_BIG_NEXT, feed.bigFrame, right, ">", -1, false);
		add(CommandFeed::FB_BIG_PREV, feed.bigFrame, right, "<", -1, false);
		return;
	}
	const SDL_Rect &drone = feed.droneFrame();
	int right = drone.x + drone.w - border;
	add(CommandFeed::FB_DRONE_MAX, drone, right, "", -1, feed.maximised == CommandFeed::MAX_DRONE);
	add(CommandFeed::FB_BOXES, drone, right, _state->tr("STR_FPS_FEED_BOXES"), -1, feed.boxes);
	add(CommandFeed::FB_NAMES, drone, right, _state->tr("STR_FPS_FEED_NAMES"), -1, feed.names);
	add(CommandFeed::FB_NIGHT, drone, right, _state->tr("STR_FPS_FEED_NIGHT"), -1, feed.nightVision);
	if (feed.maximised == CommandFeed::MAX_DRONE)
	{
		return;
	}
	if (feed.map.w > 0)
	{
		int mapRight = feed.map.x + feed.map.w - border;
		add(CommandFeed::FB_MAP_MAX, feed.map, mapRight, "", -1, false);
	}
	for (size_t window = 0; window < feed.windows.size(); ++window)
	{
		const CommandFeed::Window &shown = feed.windows[window];
		if (!shown.unit) continue;
		int windowRight = shown.frame.x + shown.frame.w - border;
		add(CommandFeed::FB_CAMERA_MAX, shown.frame, windowRight, "", (int)window, false);
	}
	// (the row's arrows, when the squad has more soldiers than windows)
	if (feed.soldiers > (int)feed.windows.size())
	{
		const int arrowW = FeedArrowW * ui;
		feed.buttons.push_back({ CommandFeed::FB_LEFT, makeRect(0, feed.row.y, arrowW, feed.row.h), "<", -1, feed.first > 0 });
		feed.buttons.push_back({ CommandFeed::FB_RIGHT, makeRect(feed.width - arrowW, feed.row.y, arrowW, feed.row.h), ">", -1,
			feed.first + (int)feed.windows.size() < feed.soldiers });
	}
}

/**
 * Puts the battle's screen right for the feed: the game's panel (its buttons, bars and words) moved under the middle
 * of the drone's picture, and the map's camera told to show its middle there (see Camera::setViewWindow): what it
 * looked at stays in the middle of what shows. With a helmet camera all over the screen neither changes (the map and
 * the panel aren't shown).
 */
void FirstPersonMode::placeFeedScreen()
{
	CommandFeed &feed = *_feed;
	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		return;
	}
	const SDL_Rect &picture = feed.dronePictureNow();
	const int shift = picture.x + picture.w / 2 - feed.width / 2;
	if (shift != feed.shift)
	{
		moveGamePanel(shift - feed.shift);
		feed.shift = shift;
	}
	if (!sameRect(picture, feed.cameraWindow))
	{
		Camera *camera = _state->_map->getCamera();
		const Position centre = camera->getCenterPosition();
		camera->setViewWindow(picture.x, picture.y, picture.w, picture.h);
		camera->centerOnPosition(centre, false);
		feed.cameraWindow = picture;
	}
}

/**
 * Moves the game's panel across (its buttons, bars and words, the warnings over it, the enemies' buttons by it): all
 * the battle's own surfaces but the map, the buttons at the screen's edges (OXCE's touch buttons, the special
 * actions' at the right), the line of debug words, and ours.
 * @param by Pixels to the right (less than 0: to the left).
 */
void FirstPersonMode::moveGamePanel(int by)
{
	if (by == 0) return;
	for (Surface *surface : _state->_surfaces)
	{
		if (surface == _state->_map || feedEdgeSurface(surface) || _ownSurfaces.count(surface)) continue;
		surface->setX(surface->getX() + by);
	}
}

/**
 * Is a surface of the battle's one at the screen's edges, which the feed leaves where it is and draws over its windows
 * (OXCE's touch buttons, the special actions' buttons at the right, the line of debug words)?
 * @param surface The surface.
 * @return True if so.
 */
bool FirstPersonMode::feedEdgeSurface(const Surface *surface) const
{
	return surface == _state->_btnCtrl || surface == _state->_btnAlt || surface == _state->_btnShift || surface == _state->_btnRMB || surface == _state->_btnMMB
		|| surface == _state->_btnPsi || surface == _state->_btnLaunch || surface == _state->_btnSpecial || surface == _state->_btnSkills || surface == _state->_txtDebug;
}

/**
 * Does the battle's blit (see blit) draw this surface of the battle's own with the feed showing? Not the map (the feed
 * draws it, in the drone's window), not those at the screen's edges (drawn after the windows, over them), and with a
 * helmet camera all over the screen only the warnings.
 * @param surface The surface.
 * @return True if the blit draws it.
 */
bool FirstPersonMode::feedDrawsGameSurface(const Surface *surface) const
{
	if (surface == _state->_map || feedEdgeSurface(surface)) return false;
	return _feed->maximised != CommandFeed::MAX_CAMERA || surface == _state->_warning;
}

/**
 * Draws what of the feed goes under the game's panel: the map in the drone's picture (as the game drew it: only that
 * part of it shows), turned into the drone's feed (see filterDrone), and the marks over the soldiers.
 * @param screen The game's screen.
 */
void FirstPersonMode::blitFeedUnder(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		return;
	}
	SDL_Rect picture = feed.dronePictureNow();
	fill(screen, picture.x, picture.y, picture.w, picture.h, feed.black);
	SDL_SetClipRect(screen, &picture);
	_state->_map->blit(screen);
	filterDrone(screen, picture);
	if (feed.names || feed.boxes)
	{
		markSoldiers(screen, picture);
	}
	SDL_SetClipRect(screen, nullptr);
}

/**
 * Turns the drone's picture into a camera's feed: black and white (or night vision's green), every other line a little
 * darker, a brighter band rolling down, specks of snow, and now and then a tear (a few lines pushed aside).
 * @param screen The game's screen.
 * @param picture The drone's picture, on it.
 */
void FirstPersonMode::filterDrone(SDL_Surface *screen, const SDL_Rect &picture)
{
	CommandFeed &feed = *_feed;
	const int left = std::max(0, (int)picture.x), top = std::max(0, (int)picture.y);
	const int right = std::min(screen->w, picture.x + (int)picture.w), bottom = std::min(screen->h, picture.y + (int)picture.h);
	if (right <= left || bottom <= top) return;
	const Uint32 now = SDL_GetTicks();
	const int rows = bottom - top;
	const int band = (int)((now / FeedBandRowMs) % (Uint32)(rows + 4 * FeedBandRows)) - 2 * FeedBandRows;
	Uint8 (*shades)[256] = feed.nightVision ? feed.green : feed.grey;
	for (int y = top; y < bottom; ++y)
	{
		const int line = y - top;
		const Uint8 *shade = shades[(line >= band && line < band + FeedBandRows) ? 2 : (line % 2 ? 1 : 0)];
		Uint8 *pixel = (Uint8 *)screen->pixels + (size_t)y * screen->pitch + left;
		for (int x = left; x < right; ++x, ++pixel)
		{
			*pixel = shade[*pixel];
		}
	}
	// The snow.
	const int specks = (right - left) * rows / FeedSnowEvery;
	for (int speck = 0; speck < specks; ++speck)
	{
		const Uint32 dice = feed.random();
		const int x = left + (int)(dice % (Uint32)(right - left)), y = top + (int)((dice >> 12) % (Uint32)rows);
		Uint8 *pixel = (Uint8 *)screen->pixels + (size_t)y * screen->pitch + x;
		*pixel = feed.nightVision ? feed.green[2][feed.snow[(dice >> 28) & 7]] : feed.snow[(dice >> 28) & 7];
	}
	// A tear now and then: a few bands of lines pushed aside.
	if (now % FeedTearEvery < (Uint32)FeedTearFor)
	{
		for (int tear = 0; tear < 3; ++tear)
		{
			const Uint32 dice = feed.random();
			const int from = top + (int)(dice % (Uint32)rows), high = 1 + (int)((dice >> 8) % 4), by = 2 + (int)((dice >> 16) % 7);
			for (int y = from; y < std::min(bottom, from + high); ++y)
			{
				Uint8 *row = (Uint8 *)screen->pixels + (size_t)y * screen->pitch;
				std::memmove(row + left + by, row + left, (size_t)(right - left - by));
			}
		}
	}
}

/**
 * The marks over our soldiers on the drone's picture (those on the level looked at or below, as the map shows them):
 * a box's corners round each, and over it their number and name (not over another's: soldiers close together show
 * the selected one's, then the first of the rest); the selected soldier's in another colour.
 * @param screen The game's screen (clipped to the picture).
 * @param picture The drone's picture.
 */
void FirstPersonMode::markSoldiers(SDL_Surface *screen, const SDL_Rect &picture)
{
	CommandFeed &feed = *_feed;
	Map *map = _state->_map;
	Camera *camera = map->getCamera();
	const int ui = feed.ui, corner = FeedCornerLength;
	// (the selected soldier's first: a name that would cover one already written is left out, the box kept)
	std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
	std::stable_partition(squad.begin(), squad.end(), [this](const std::pair<BattleUnit*, int> &member) { return member.first == _save->getSelectedUnit(); });
	std::vector<SDL_Rect> written;
	for (const std::pair<BattleUnit*, int> &member : squad)
	{
		BattleUnit *unit = member.first;
		if (unit->getPosition().z > camera->getViewLevel()) continue;
		// Where the map draws them (see Map's arrow over the selected unit): the tile, the steps' offset, their height.
		Position screenPosition;
		camera->convertMapToScreen(unit->getPosition(), &screenPosition);
		screenPosition += camera->getMapOffset();
		const Position offset = map->calculateWalkingOffset(unit).ScreenOffset;
		const int size = unit->getArmor()->getSize();
		const int middle = screenPosition.x + offset.x + 16;
		const int feet = screenPosition.y + offset.y + Position::TileZ + 8 + (size > 1 ? 8 : 0);
		const int head = feet - 8 - (unit->getHeight() + unit->getFloatHeight()) - (size > 1 ? 4 : 0);
		const int half = size > 1 ? 22 : 9;
		const SDL_Rect box = makeRect(middle - half, head - 3, 2 * half, feet - head + 5);
		if (box.x > picture.x + picture.w || box.x + box.w < picture.x || box.y > picture.y + picture.h || box.y + box.h + 0 < picture.y - FeedWordsH * ui) continue;
		const Uint8 colour = unit == _save->getSelectedUnit() ? feed.pickedColour : feed.hudColour;
		if (feed.boxes)
		{
			// (the corners only, as a camera's tracker marks what it follows)
			const int x0 = box.x, y0 = box.y, x1 = box.x + box.w - 1, y1 = box.y + box.h - 1;
			fill(screen, x0, y0, corner, 1, colour); fill(screen, x0, y0, 1, corner, colour);
			fill(screen, x1 - corner + 1, y0, corner, 1, colour); fill(screen, x1, y0, 1, corner, colour);
			fill(screen, x0, y1, corner, 1, colour); fill(screen, x0, y1 - corner + 1, 1, corner, colour);
			fill(screen, x1 - corner + 1, y1, corner, 1, colour); fill(screen, x1, y1 - corner + 1, 1, corner, colour);
		}
		if (feed.names)
		{
			const std::string words = std::to_string(member.second) + " " + unit->getName(_game->getLanguage(), false);
			feed.writer->setText(words);
			const int wide = feed.writer->getTextWidth() * ui;
			const SDL_Rect label = makeRect(middle - wide / 2 - ui, box.y - FeedWordsH * ui - 1 - ui, wide + 2 * ui, FeedWordsH * ui + ui);
			const bool covers = std::any_of(written.begin(), written.end(), [&label](const SDL_Rect &other) {
				return label.x < other.x + (int)other.w && other.x < label.x + (int)label.w && label.y < other.y + (int)other.h && other.y < label.y + (int)label.h;
			});
			if (!covers)
			{
				feedWrite(screen, words, middle - wide / 2, box.y - FeedWordsH * ui - 1, colour, 0, true);
				written.push_back(label);
			}
		}
	}
}

/**
 * Words on the screen in the small font, magnified as the feed is.
 * @param screen The game's screen.
 * @param text The words.
 * @param x, y Their top left corner.
 * @param colour Their colour (the bright end of the run the letters shade down from).
 * @param room The most pixels they may take across (0: any); longer, they are cut short with two dots.
 * @param backed True to draw them on a dark ground.
 * @return The pixels they took across.
 */
int FirstPersonMode::feedWrite(SDL_Surface *screen, const std::string &text, int x, int y, Uint8 colour, int room, bool backed)
{
	CommandFeed &feed = *_feed;
	Text &writer = *feed.writer;
	const int ui = feed.ui;
	std::string words = text;
	writer.setText(words);
	if (room > 0 && writer.getTextWidth() * ui > room)
	{
		// (cut short a letter at a time; the words are short)
		while (!words.empty())
		{
			do { words.pop_back(); } while (!words.empty() && ((unsigned char)words.back() & 0xC0) == 0x80); // (whole UTF-8 letters)
			writer.setText(words + "..");
			if (writer.getTextWidth() * ui <= room) break;
		}
		words += "..";
		writer.setText(words);
	}
	writer.setColor(colour);
	writer.draw();
	const int letters = writer.getTextWidth();
	if (backed)
	{
		fill(screen, x - ui, y - ui, letters * ui + 2 * ui, FeedWordsH * ui + ui, feed.titleBack);
	}
	for (int row = 0; row < writer.getHeight(); ++row)
	{
		for (int column = 0; column <= letters && column < writer.getWidth(); ++column)
		{
			const Uint8 pixel = writer.getPixel(column, row);
			if (pixel) fill(screen, x + column * ui, y + row * ui, ui, ui, pixel);
		}
	}
	return letters * ui;
}

/**
 * A window's frame: its border and its title line's ground.
 * @param screen The game's screen.
 * @param frame The window.
 * @param borderColour The border's colour (lit for the selected soldier's camera, red for one popped up).
 */
void FirstPersonMode::drawFeedWindow(SDL_Surface *screen, const SDL_Rect &frame, Uint8 borderColour)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, border = FeedBorder * ui;
	outline(screen, frame, border, borderColour);
	fill(screen, frame.x + border, frame.y + border, frame.w - 2 * border, FeedTitleH * ui, feed.titleBack);
}

/**
 * Draws the feed's windows over the battle's screen: the row of helmet cameras (each titled with the soldier's number
 * and name, a red dot that blinks, their health and time units over the picture), its arrows, the drone's frame and
 * title (the turn and the time), the minimap; or the window all over the screen. Then the battle's buttons at the
 * screen's edges, over all of it.
 * @param screen The game's screen.
 */
void FirstPersonMode::blitFeedOver(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, border = FeedBorder * ui, titleH = FeedTitleH * ui, wordsY = border + (titleH - FeedWordsH * ui) / 2 + ui;
	const bool blink = SDL_GetTicks() % 1000 < 600;
	// (a title's words: after the live dot, up to the first button)
	auto titleWords = [&](const SDL_Rect &frame, const std::string &words, Uint8 colour, bool dot) {
		int x = frame.x + border + 2 * ui;
		if (dot)
		{
			if (blink) fill(screen, x, frame.y + border + titleH / 2 - 2 * ui, 4 * ui, 4 * ui, feed.liveColour);
			x += 6 * ui;
		}
		int room = frame.x + frame.w - border - x;
		for (const CommandFeed::Button &button : feed.buttons)
		{
			if (button.box.y == frame.y + border + FeedButtonInset * ui && button.box.x >= x && button.box.x < frame.x + frame.w) room = std::min(room, button.box.x - x - 2 * ui);
		}
		return x + feedWrite(screen, words, x, frame.y + wordsY, colour, room);
	};
	// (why the aliens' turn popped a soldier's camera up, after their name, in red)
	const BattleUnit *alerted = _alertUnit && SDL_GetTicks() < _alertUntil && !_alertText.empty() ? _alertUnit : nullptr;
	auto alertWords = [&](const SDL_Rect &frame, int after) {
		feedWrite(screen, _state->tr(_alertText), after + 4 * ui, frame.y + wordsY, feed.liveColour, std::max(0, frame.x + (int)frame.w - border - titleH - after - 6 * ui));
	};
	const std::string live = _state->tr("STR_FPS_FEED_LIVE");

	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		drawFeedWindow(screen, feed.bigFrame, feed.bigUnit == alerted ? feed.liveColour : feed.frameLit);
		if (feed.big)
		{
			feed.big->setX(feed.bigPicture.x);
			feed.big->setY(feed.bigPicture.y);
			feed.big->blit(screen);
		}
		const std::pair<BattleUnit*, int> *member = nullptr;
		const std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
		for (const auto &each : squad) if (each.first == feed.bigUnit) member = &each;
		const int after = titleWords(feed.bigFrame, (member ? std::to_string(member->second) + " " : std::string()) + (feed.bigUnit ? feed.bigUnit->getName(_game->getLanguage(), false) : std::string()) + "  " + live, feed.wordsColour, true);
		if (feed.bigUnit && feed.bigUnit == alerted) alertWords(feed.bigFrame, after);
	}
	else
	{
		// The row of helmet cameras (not under the drone's feed all over the screen).
		for (CommandFeed::Window &window : feed.windows)
		{
			if (feed.maximised != CommandFeed::MAX_NONE) break;
			const bool selected = window.unit && window.unit == _save->getSelectedUnit();
			drawFeedWindow(screen, window.frame, window.unit && window.unit == alerted ? feed.liveColour : selected ? feed.frameLit : feed.frameColour);
			if (!window.unit)
			{
				continue;
			}
			window.view->setX(window.picture.x);
			window.view->setY(window.picture.y);
			window.view->blit(screen);
			const int after = titleWords(window.frame, std::to_string(window.number) + " " + window.unit->getName(_game->getLanguage(), false), selected ? feed.pickedColour : feed.wordsColour, true);
			if (window.unit == alerted) alertWords(window.frame, after);
			// (their health and time units over the picture's foot, as a helmet camera's feed is overlaid)
			const std::string state = _state->tr("STR_FPS_FEED_STATE").arg(window.unit->getHealth()).arg(window.unit->getTimeUnits());
			SDL_SetClipRect(screen, &window.picture);
			feedWrite(screen, state, window.picture.x + 2 * ui, window.picture.y + window.picture.h - FeedWordsH * ui - ui, feed.hudColour, 0, true);
			SDL_SetClipRect(screen, nullptr);
		}

		// The drone's frame round its picture (the game's panel at its foot) and its title.
		const SDL_Rect &drone = feed.droneFrame();
		outline(screen, makeRect(drone.x, drone.y, drone.w, drone.h + border), border, feed.frameColour); // (no foot: the panel is there)
		fill(screen, drone.x + border, drone.y + border, drone.w - 2 * border, titleH, feed.titleBack);
		char clock[16] = "";
		const std::time_t seconds = std::time(nullptr);
		if (const std::tm *local = std::localtime(&seconds)) std::strftime(clock, sizeof(clock), "%H:%M:%S", local);
		titleWords(drone, _state->tr("STR_FPS_FEED_DRONE").arg(_save->getTurn()).arg(clock), feed.wordsColour, true);

		// The minimap.
		if (feed.maximised == CommandFeed::MAX_NONE && feed.map.w > 0)
		{
			drawFeedWindow(screen, feed.map, feed.frameColour);
			titleWords(feed.map, _state->tr("STR_FPS_FEED_MAP"), feed.wordsColour, false);
			if (feed.minimap) feed.minimap->blit();
		}
	}

	// The buttons: their words or signs (all over the screen: a square; back: a square in a square), lit when on.
	for (const CommandFeed::Button &button : feed.buttons)
	{
		const bool arrow = button.kind == CommandFeed::FB_LEFT || button.kind == CommandFeed::FB_RIGHT;
		if (arrow && !button.lit) continue; // (an arrow that can't scroll further isn't shown)
		const Uint8 colour = button.lit && !arrow ? feed.frameLit : feed.frameColour;
		fill(screen, button.box.x, button.box.y, button.box.w, button.box.h, feed.titleBack);
		outline(screen, button.box, ui, colour);
		if (!button.words.empty())
		{
			feed.writer->setText(button.words);
			const int wide = feed.writer->getTextWidth() * ui;
			feedWrite(screen, button.words, button.box.x + (button.box.w - wide) / 2, button.box.y + (button.box.h - FeedWordsH * ui) / 2 + ui, button.lit ? feed.wordsColour : feed.dimColour);
		}
		else
		{
			const int margin = 2 * ui, inner = button.box.h - 2 * margin;
			SDL_Rect sign = makeRect(button.box.x + (button.box.w - inner) / 2, button.box.y + margin, inner, inner);
			outline(screen, sign, ui, feed.wordsColour);
			fill(screen, sign.x, sign.y, sign.w, ui + ui / 2 + 1, feed.wordsColour); // (a window's title bar)
			if (button.kind == CommandFeed::FB_BIG_RESTORE || (button.kind == CommandFeed::FB_DRONE_MAX && button.lit))
			{
				fill(screen, sign.x + 2 * ui, sign.y + 3 * ui, std::max(1, (int)sign.w - 4 * ui), std::max(1, (int)sign.h - 5 * ui), feed.wordsColour);
			}
		}
	}

	// The battle's buttons at the screen's edges, over the windows.
	for (Surface *surface : _state->_surfaces)
	{
		if (feedEdgeSurface(surface)) surface->blit(screen);
	}
}

/**
 * A key while the feed shows: Escape puts a window shown all over the screen back (both its press and its release
 * are then the feed's). The rest are the battle's.
 * @param action The key.
 * @return True if it was the feed's.
 */
bool FirstPersonMode::handleFeedKey(Action *action)
{
	if (!feedShown() || _feed->maximised == CommandFeed::MAX_NONE || action->getDetails()->key.keysym.sym != Options::keyCancel)
	{
		return false;
	}
	if (action->getDetails()->type == SDL_KEYDOWN)
	{
		_feed->maximised = CommandFeed::MAX_NONE;
		_feed->big.reset();
		_feed->bigUnit = nullptr;
	}
	return true;
}

/**
 * The mouse while the feed shows (its moves, buttons and wheel), after our top bar's buttons had theirs:
 *  - a press on the drone's picture is the map's until the button comes up (a drag scrolls on past the window);
 *  - the battle's buttons at the screen's edges, theirs;
 *  - the feed's buttons: a left press works one;
 *  - with a helmet camera all over the screen, nothing else (the wheel: the soldier before or the next);
 *  - the minimap: its own (all the tactical map's buttons, its drag and wheel);
 *  - the row of cameras: a left click selects the soldier (the drone's feed goes to them), the wheel scrolls the row;
 *  - the drone's picture: the map's and the panel's, as on the isometric map;
 *  - anywhere else, the battle's without the map (see feedWithoutMap), and the map stops scrolling at its edge.
 * @param action The mouse's event.
 * @return True if it was the feed's (false: the battle's as it comes).
 */
bool FirstPersonMode::handleFeedMouse(Action *action)
{
	CommandFeed &feed = *_feed;
	const SDL_Event &event = *action->getDetails();
	int eventX = 0, eventY = 0;
	if (event.type == SDL_MOUSEMOTION)
	{
		eventX = event.motion.x;
		eventY = event.motion.y;
	}
	else
	{
		mouseEventPosition(event, eventX, eventY);
	}
	action->setMouseAction(eventX, eventY, 0, 0);
	const int x = (int)std::floor(action->getAbsoluteXMouse()), y = (int)std::floor(action->getAbsoluteYMouse());
	const bool press = event.type == SDL_MOUSEBUTTONDOWN && wheelStep(event) == 0, leftPress = press && event.button.button == SDL_BUTTON_LEFT;
	const int wheel = wheelStep(event);

	if (feed.pressedOnDrone)
	{
		if (event.type == SDL_MOUSEBUTTONUP) feed.pressedOnDrone = false;
		return false;
	}
	const bool onDrone = feed.maximised != CommandFeed::MAX_CAMERA && inside(feed.dronePictureNow(), x, y);
	if (!onDrone && feed.overDrone)
	{
		_state->_map->getCamera()->stopMouseScrolling(); // (it scrolled at the window's edge: the mouse left it)
	}
	feed.overDrone = onDrone;

	for (Surface *surface : _state->_surfaces)
	{
		if (feedEdgeSurface(surface) && surface->getVisible() && x >= surface->getX() && y >= surface->getY() && x < surface->getX() + surface->getWidth() && y < surface->getY() + surface->getHeight())
		{
			return feedWithoutMap(action);
		}
	}
	for (size_t button = 0; button < feed.buttons.size(); ++button)
	{
		if (inside(feed.buttons[button].box, x, y))
		{
			if (leftPress) pressFeedButton((int)button);
			return true;
		}
	}
	if (feed.maximised == CommandFeed::MAX_CAMERA)
	{
		if (wheel != 0)
		{
			for (const CommandFeed::Button &button : feed.buttons)
			{
				if (button.kind == (wheel > 0 ? CommandFeed::FB_BIG_PREV : CommandFeed::FB_BIG_NEXT)) pressFeedButton((int)(&button - feed.buttons.data()));
			}
		}
		return true;
	}
	if (feed.maximised == CommandFeed::MAX_NONE && feed.minimap && inside(feed.mapPicture, x, y))
	{
		feed.minimap->handle(action);
		return true;
	}
	if (feed.maximised == CommandFeed::MAX_NONE && inside(feed.row, x, y))
	{
		if (wheel != 0)
		{
			scrollFeed(-wheel);
		}
		else if (leftPress)
		{
			for (CommandFeed::Window &window : feed.windows)
			{
				if (window.unit && inside(window.frame, x, y)) selectUnit(window.unit);
			}
		}
		return true;
	}
	if (onDrone)
	{
		if (press) feed.pressedOnDrone = true;
		return false;
	}
	return feedWithoutMap(action);
}

/**
 * Hands a mouse event to the battle (its panel's buttons, its edge buttons) without the map seeing it: the map is
 * hidden while the battle has it (it's under the feed's windows there), and the mode is told not to take it again.
 * @param action The event.
 * @return True (it was dealt with).
 */
bool FirstPersonMode::feedWithoutMap(Action *action)
{
	Map *map = _state->_map;
	map->setHidden(true);
	_feedDispatching = true;
	_state->handle(action);
	_feedDispatching = false;
	map->setHidden(false);
	return true;
}

/**
 * One of the feed's buttons pressed (with the buttons' click).
 * @param index The button's place in the list.
 */
void FirstPersonMode::pressFeedButton(int index)
{
	CommandFeed &feed = *_feed;
	const CommandFeed::Button button = feed.buttons[index];
	if (TextButton::soundPress) TextButton::soundPress->play(Mix_GroupAvailable(0));
	const std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
	switch (button.kind)
	{
	case CommandFeed::FB_NIGHT: feed.nightVision = !feed.nightVision; break;
	case CommandFeed::FB_NAMES: feed.names = !feed.names; break;
	case CommandFeed::FB_BOXES: feed.boxes = !feed.boxes; break;
	case CommandFeed::FB_DRONE_MAX: feed.maximised = feed.maximised == CommandFeed::MAX_DRONE ? CommandFeed::MAX_NONE : CommandFeed::MAX_DRONE; break;
	case CommandFeed::FB_MAP_MAX: showTacticalMap(); break;
	case CommandFeed::FB_CAMERA_MAX:
		if (button.camera >= 0 && button.camera < (int)feed.windows.size() && feed.windows[button.camera].unit)
		{
			feed.maximised = CommandFeed::MAX_CAMERA;
			feed.bigUnit = feed.windows[button.camera].unit;
		}
		break;
	case CommandFeed::FB_LEFT: scrollFeed(-1); break;
	case CommandFeed::FB_RIGHT: scrollFeed(1); break;
	case CommandFeed::FB_BIG_PREV:
	case CommandFeed::FB_BIG_NEXT:
		for (size_t place = 0; place < squad.size(); ++place)
		{
			if (squad[place].first != feed.bigUnit) continue;
			const int by = button.kind == CommandFeed::FB_BIG_NEXT ? 1 : -1;
			feed.bigUnit = squad[(place + squad.size() + by) % squad.size()].first;
			break;
		}
		break;
	case CommandFeed::FB_BIG_RESTORE:
		feed.maximised = CommandFeed::MAX_NONE;
		feed.big.reset();
		feed.bigUnit = nullptr;
		break;
	}
	feedButtons();
}

/**
 * Scrolls the row of helmet cameras through the squad.
 * @param by Soldiers on (less than 0: back).
 */
void FirstPersonMode::scrollFeed(int by)
{
	CommandFeed &feed = *_feed;
	feed.first = std::clamp(feed.first + by, 0, std::max(0, feed.soldiers - (int)feed.windows.size()));
}

}
