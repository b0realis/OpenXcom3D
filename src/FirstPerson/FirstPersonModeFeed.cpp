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
 * FirstPersonModeFeed.cpp: the Live command feed (setting "Live command feed", experimental): the battle as a command
 * post's screens. The owner's design (police tactical units' helmet cameras streaming to a command post), and then:
 * "only one larger selected window ... you could control the soldier in his larger selected window and drone feed and
 * tac map would update in real time", "let it be performant!", "Equipment windows should be just inventory! And always
 * keep the small buttons on the top with soldier mini cam Cards". Under the top bar (its squad buttons, their cards with
 * the helmet camera) the selected soldier's inventory (the game's own, items moved there as on its screen) and their 3D
 * view in a window, where the soldier is played as in the full 3D view (the view made at the window's size: the only
 * 3D view drawn); below, the battle's own map as a drone's feed (black and white with static, or night vision; each
 * soldier's number and name over them, a box round each), with the game's panel under it, and the tactical map beside
 * it. The drone, the tactical map and the inventory go on as the soldier moves and fires.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <SDL_mixer.h>
#include "FirstPersonApi.h"
#include "FirstPersonMapState.h"
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Inventory.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/Options.h"
#include "../Engine/Sound.h"
#include "../Interface/Bar.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/RuleSoldier.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

namespace
{

/// The windows' borders are this thick, their title lines this high, the gaps between the helmet cameras this wide
/// (all times the screen's magnification); a line of words is this high, a title's button this far inside its line.
const int FeedBorder = 1, FeedTitleH = 11, FeedGap = 2, FeedWordsH = 9, FeedButtonInset = 1, FeedButtonPad = 3;
/// The inventory window and the 3D view's reach this share of the screen's height down; the inventory window takes at
/// most this share of the width; the inventory's screen is this big (the game's: its slots' places are set for it).
const double FeedUpperShare = 0.55, FeedInventoryShare = 0.45;
const int FeedInventoryScreenW = 320, FeedInventoryScreenH = 200;
/// The buttons the inventory screen's background draws (InventoryState's): the soldier before and the next, unload,
/// and the ground's scroll; x, y, width, height on its screen.
const int FeedInventoryPrevious[4] = { 273, 1, 23, 22 }, FeedInventoryNext[4] = { 297, 1, 23, 22 }, FeedInventoryUnload[4] = { 288, 32, 32, 25 },
	FeedInventoryGround[4] = { 289, 137, 32, 15 };
/// The drone's window takes this share of the screen's width, the minimap the rest, if that is at least FeedMapLeast
/// wide (else the drone has it all).
const double FeedDroneShare = 0.6;
const int FeedMapLeast = 120;
/// The drone's picture: a speck of snow in every this many pixels; a band this many rows high, a little brighter,
/// rolls down it this many milliseconds a row; every this many milliseconds the picture tears for this long.
const int FeedSnowEvery = 70, FeedBandRows = 5, FeedBandRowMs = 30, FeedTearEvery = 3700, FeedTearFor = 140;
/// The marks over a soldier on the drone's picture: the corners of the box this long.
const int FeedCornerLength = 4;
/// The drone floats (stabiliser off): this many pixels (times the magnification) at the most either way, its sways
/// this many milliseconds round.
const int FeedSwayMost = 3;
/// The drone's arrows at its picture's edges: this long and this deep (times the magnification), this far in; held,
/// it flies this many pixels a second. FLY TO glides to the soldier in this many milliseconds. The level buttons at the
/// right are this big.
const int FeedArrowLong = 14, FeedArrowDeep = 7, FeedArrowInset = 3, FeedFlySpeed = 260, FeedFlyMs = 700, FeedLevelButton = 12;
/// FOLLOW: the drone closes this share of the way to its soldier each second (smoothly, as the time between frames
/// allows), and stops short of a pixel.
const double FeedFollowRate = 5.0;
const double FeedSwayPeriods[4] = { 5300.0, 2100.0, 6900.0, 2900.0 };

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
 * The Live command feed's state (see FirstPersonMode::updateFeed): where its windows are, the inventory window's
 * inventory and picture, the minimap, the drone's switches, what of the battle's screen it changed, and its colours.
 */
struct CommandFeed
{
	/// Which window is all over the screen (the 3D view's: the full 3D view, out of the feed).
	enum Maximised { MAX_NONE, MAX_DRONE };
	/// A button: what it does, its place on the screen, its words (none: a drawn sign), a value, and whether it shows
	/// lit (a switch that is on).
	enum ButtonKind { FB_NIGHT, FB_THERMAL, FB_STABILISER, FB_NAMES, FB_BOXES, FB_DRONE_MAX, FB_FLY_TO, FB_FOLLOW, FB_MOVE_UP, FB_MOVE_DOWN, FB_MOVE_LEFT, FB_MOVE_RIGHT,
		FB_LEVEL_UP, FB_LEVEL_DOWN, FB_MAP_MAX, FB_VIEW_MAX, FB_INVENTORY };
	/// How the drone sees: black and white; night vision, all of it green (realistic) or the units and items in their own
	/// colours (enhanced); thermal, the units, explosions and fire white hot on a cool grey world.
	enum Vision { VISION_PLAIN, VISION_NIGHT, VISION_NIGHT_PLUS, VISION_THERMAL };
	struct Button
	{
		ButtonKind kind;
		SDL_Rect box;
		std::string words;
		int value;
		bool lit;
	};
	// The layout and what it was made for (the screen's size, the top bar's foot, the squad's count).
	int width = 0, height = 0, top = -1, soldiers = -1, ui = 1;
	SDL_Rect inventory{}, inventoryPicture{}, inventoryShown{}, view{}, viewPicture{}, drone{}, dronePicture{}, map{}, mapPicture{}, bigFrame{}, bigPicture{};
	std::vector<Button> buttons;
	// Flying the drone: the arrow held (one of FB_MOVE_*, else none), and a glide to a soldier (FLY TO): from where to
	// where (the map's offsets), since when; the mouse's last point (to light what it is over); the last frame's time.
	int moving = -1;
	bool gliding = false, follow = false;
	Position glideFrom, glideTo;
	Uint32 glideStart = 0, lastFrame = 0;
	int mouseX = -1, mouseY = -1;
	Maximised maximised = MAX_NONE;
	// The minimap: the tactical map in its window (made again only for another window).
	std::unique_ptr<FirstPersonMapState> minimap;
	SDL_Rect minimapWindow{};
	// The inventory window: the game's own inventory (its grid and items, moved by the mouse as on its screen), drawn
	// with the screen's background and the soldier's figure into a picture of the screen's size, shown scaled into the
	// window (source columns and rows for each shown one); whose, and what of theirs it was last arranged for.
	std::unique_ptr<Inventory> inventoryGrid;
	std::unique_ptr<Surface> inventoryScreen, doll;
	std::vector<int> inventoryColumns, inventoryRows;
	BattleUnit *inventoryUnit = nullptr;
	std::string inventoryKey;
	// The drone's switches: how it sees, the stabiliser (off: it floats a little), the soldiers' names, the boxes round
	// them; how far it floats this frame; what of the map's picture is units, items and effects (see Map::setMaskTarget).
	Vision vision = VISION_PLAIN;
	bool stabiliser = false, names = true, boxes = true;
	int swayX = 0, swayY = 0;
	std::unique_ptr<Surface> mask;
	// What it changed on the battle's screen: the game's panel hidden (and where its warnings stood), the map's camera
	// in this window (see Camera::setViewWindow); shown this frame.
	bool panelHidden = false;
	int warningX = 0;
	SDL_Rect cameraWindow{};
	bool on = false;
	// The mouse: a press on the drone's picture, the map's until it comes up; over the drone's picture last.
	bool pressedOnDrone = false, overDrone = false;
	// The colours, from the battle's palette (made again if it changes): the drone's black and white and night vision
	// (plain, a scan line's darker and the rolling band's brighter), the snow, the windows' own and the cards' bars.
	SDL_Color palette[256];
	bool paletteKnown = false;
	Uint8 grey[3][256], green[3][256], cool[3][256], hot[3][256], snow[8];
	Uint8 black = 0, frameColour = 0, frameLit = 0, titleBack = 0, wordsColour = 0, dimColour = 0, liveColour = 0, hudColour = 0, pickedColour = 0;
	Uint8 purple = 0, purpleLit = 0;
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

namespace
{

/// Is a button one of the drone's (shown, and clicked, with its picture all over the screen too)?
bool droneButton(CommandFeed::ButtonKind kind)
{
	return kind == CommandFeed::FB_NIGHT || kind == CommandFeed::FB_THERMAL || kind == CommandFeed::FB_STABILISER || kind == CommandFeed::FB_NAMES
		|| kind == CommandFeed::FB_BOXES || kind == CommandFeed::FB_DRONE_MAX || kind == CommandFeed::FB_FLY_TO || kind == CommandFeed::FB_FOLLOW || kind == CommandFeed::FB_MOVE_UP
		|| kind == CommandFeed::FB_MOVE_DOWN || kind == CommandFeed::FB_MOVE_LEFT || kind == CommandFeed::FB_MOVE_RIGHT || kind == CommandFeed::FB_LEVEL_UP
		|| kind == CommandFeed::FB_LEVEL_DOWN;
}

/// Is a button one of the drone's arrows at its picture's edges?
bool moveArrow(CommandFeed::ButtonKind kind)
{
	return kind == CommandFeed::FB_MOVE_UP || kind == CommandFeed::FB_MOVE_DOWN || kind == CommandFeed::FB_MOVE_LEFT || kind == CommandFeed::FB_MOVE_RIGHT;
}

/// A filled triangle in a box, pointing up, down, left or right (0 to 3).
void triangle(SDL_Surface *screen, const SDL_Rect &box, int pointing, Uint8 colour)
{
	if (pointing < 2)
	{
		for (int row = 0; row < box.h; ++row)
		{
			const int depth = pointing == 0 ? row : box.h - 1 - row, half = (int)box.w * (depth + 1) / (2 * box.h);
			fill(screen, box.x + box.w / 2 - half, box.y + row, std::max(1, 2 * half), 1, colour);
		}
	}
	else
	{
		for (int column = 0; column < box.w; ++column)
		{
			const int depth = pointing == 2 ? column : box.w - 1 - column, half = (int)box.h * (depth + 1) / (2 * box.w);
			fill(screen, box.x + column, box.y + box.h / 2 - half, 1, std::max(1, 2 * half), colour);
		}
	}
}

}

/**
 * Does the Live command feed take the screen? With its setting on, unless the player went to the full 3D view in their
 * turn (the 3D key, or the 3D window's button: see toggleView; the enemy's turn is always watched in the feed, the owner:
 * "On enemy turn it goes to normal 3d enemy turn"); never in VR mode, whose headset shows the 3D view.
 * @return True if so.
 */
bool FirstPersonMode::feedWanted() const
{
	return Options::fpsCommandFeed && (_feedPreferred || _save->getSide() != FACTION_PLAYER) && !vrOn();
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
 * The 3D view's size while the feed shows: its window's picture (see wantedViewSize). The view itself is made that
 * size, so the feed costs no more than one 3D view at the window's size.
 * @param width Set to the picture's width, in the game's screen pixels.
 * @param height Set to its height.
 * @return True if the feed is wanted and laid out (else the sizes are left alone).
 */
bool FirstPersonMode::feedViewSize(int &width, int &height) const
{
	if (!feedWanted() || !_feed || _feed->viewPicture.w < 8 || _feed->viewPicture.h < 8)
	{
		return false;
	}
	width = _feed->viewPicture.w;
	height = _feed->viewPicture.h;
	return true;
}

/**
 * Where the 3D view's picture stands on the game's screen while the feed shows it in its window: for what is drawn over
 * a point of the view (the target's name under the crosshair, see canvasPixelX).
 * @param picture Set to the picture's place on the screen.
 * @return True if the feed shows the view in its window (else it is left alone).
 */
bool FirstPersonMode::feedViewPicture(SDL_Rect &picture) const
{
	if (!feedShown() || _feed->maximised != CommandFeed::MAX_NONE || _feed->viewPicture.w <= 0 || _feed->viewPicture.h <= 0)
	{
		return false;
	}
	picture = _feed->viewPicture;
	return true;
}

/**
 * Each frame (see think): the Live command feed, when wanted, laid out for the screen and the squad; the battle's
 * screen put right for it (the map shown in the drone's window while the 3D view plays in its own, the game's panel
 * under the drone, the map's camera in the drone's window, following the soldier looked through); the crosshair in the
 * 3D window's middle; the inventory window's soldier and items; the minimap's frame. Not wanted: the battle's screen as
 * it was.
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
	_wants3D = true; // (the 3D view plays in its window: up as soon as there is a soldier to look through, see think)
	feedColours();
	layOutFeed();

	// The map is the drone's feed, the 3D view or not; the game's warnings at their place over its panel.
	if (!_state->_map->getVisible())
	{
		_state->_map->setVisible(true);
		_state->_map->invalidate();
	}

	BattleUnit *selected = _save->getSelectedUnit();
	feedButtons();
	placeFeedScreen();

	// What of the map's picture is units, items and effects, for thermal and enhanced night vision: the map draws them
	// alone too, into the mask, each time it draws (see Map::setMaskTarget).
	Map *map = _state->_map;
	const bool masked = feed.vision == CommandFeed::VISION_THERMAL || feed.vision == CommandFeed::VISION_NIGHT_PLUS;
	if (masked && (!feed.mask || feed.mask->getWidth() != map->getWidth() || feed.mask->getHeight() != map->getHeight()))
	{
		feed.mask.reset(new Surface(map->getWidth(), map->getHeight(), 0, 0));
		feed.mask->setPalette(_state->getPalette());
		map->invalidate();
	}
	map->setMaskTarget(masked ? feed.mask.get() : nullptr, feed.vision == CommandFeed::VISION_NIGHT_PLUS);

	// The drone stays where the player flies it (the owner: "Otherwise it stays as player set it"): an arrow held flies it
	// that way; FLY TO glides it to the soldier (see flyFeedTo).
	Camera *camera = map->getCamera();
	const Uint32 now = SDL_GetTicks();
	const double seconds = feed.lastFrame ? std::min<Uint32>(100, now - feed.lastFrame) / 1000.0 : 0.0;
	feed.lastFrame = now;
	if (feed.moving >= 0 && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)))
	{
		feed.moving = -1; // (let go somewhere the feed didn't hear)
	}
	if (feed.moving >= 0)
	{
		const int step = (int)std::lround(FeedFlySpeed * feed.ui * seconds);
		const int byX = feed.moving == CommandFeed::FB_MOVE_LEFT ? step : feed.moving == CommandFeed::FB_MOVE_RIGHT ? -step : 0;
		const int byY = feed.moving == CommandFeed::FB_MOVE_UP ? step : feed.moving == CommandFeed::FB_MOVE_DOWN ? -step : 0;
		if (byX || byY) camera->scrollXY(byX, byY, true);
		feed.gliding = false;
	}
	if (feed.gliding)
	{
		const double done = std::min(1.0, (now - feed.glideStart) / (double)FeedFlyMs), eased = done * done * (3.0 - 2.0 * done);
		const Position at = camera->getMapOffset();
		const int wantX = (int)std::lround(feed.glideFrom.x + (feed.glideTo.x - feed.glideFrom.x) * eased);
		const int wantY = (int)std::lround(feed.glideFrom.y + (feed.glideTo.y - feed.glideFrom.y) * eased);
		if (wantX != at.x || wantY != at.y) camera->scrollXY(wantX - at.x, wantY - at.y, true);
		if (done >= 1.0) feed.gliding = false;
	}
	// FOLLOW (the owner: "a button to always follow the selected soldier"): in the player's turn the drone keeps after
	// the soldier in the 3D window, on their level, closing the way smoothly (the selected one, another when another is
	// selected). Not in the enemy's turn: the game's own camera goes after the aliens then.
	const BattleUnit *followed = isActive() && _trackedUnit ? _trackedUnit : _save->getSelectedUnit();
	if (feed.follow && !feed.gliding && feed.moving < 0 && _save->getSide() == FACTION_PLAYER && followed && followed->getFaction() == FACTION_PLAYER
		&& !followed->isOut() && followed->getTile())
	{
		const Position at = camera->getMapOffset();
		camera->centerOnPosition(followed->getPosition(), false);
		const Position target = camera->getMapOffset();
		camera->setMapOffset(Position(at.x, at.y, target.z));
		const double share = std::min(1.0, seconds * FeedFollowRate);
		const int byX = (int)std::lround((target.x - at.x) * share), byY = (int)std::lround((target.y - at.y) * share);
		// (the last pixels at once, not crawled over)
		camera->scrollXY(std::abs(target.x - at.x) <= 1 ? target.x - at.x : byX, std::abs(target.y - at.y) <= 1 ? target.y - at.y : byY, true);
	}

	// The crosshair (and the words beside it, placed by it: see updateCrosshair) in the middle of the 3D window.
	const SDL_Rect &picture = feed.viewPicture;
	_crosshair->setX((int)std::lround(toCanvasX(picture.x + picture.w / 2.0)) - CrosshairSize / 2);
	_crosshair->setY((int)std::lround(toCanvasY(picture.y + picture.h / 2.0)) - CrosshairSize / 2);

	updateFeedInventory(selected && selected->getFaction() == FACTION_PLAYER && !selected->isOut() && selected->getTile() ? selected : nullptr);

	// The minimap's frame (its clock, drawn again as the battle goes on).
	if (feed.minimap && feed.maximised == CommandFeed::MAX_NONE)
	{
		feed.minimap->think();
	}
}

/**
 * The Live command feed leaves the screen (the full 3D view, or its setting went off): the game's panel back, the map's
 * camera on the whole screen again around what it looked at, the map hidden again under the 3D view,
 * the crosshair back in the middle, and the feed's windows let go (made again when it comes back).
 */
void FirstPersonMode::leaveFeed()
{
	CommandFeed &feed = *_feed;
	hideGamePanel(false);
	_state->_map->setMaskTarget(nullptr, false);
	Camera *camera = _state->_map->getCamera();
	if (feed.cameraWindow.w > 0)
	{
		const Position centre = camera->getCenterPosition();
		camera->setViewWindow(0, 0, 0, 0);
		camera->centerOnPosition(centre, false);
		feed.cameraWindow = SDL_Rect{};
	}
	camera->stopMouseScrolling();
	_state->_map->setVisible(!isActive());
	_state->_warning->setX(feed.warningX);
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	_crosshair->setX(canvasWidth() / 2 - CrosshairSize / 2);
	_crosshair->setY(canvasHeight() / 2 - CrosshairSize / 2);
	feed.minimap.reset();
	feed.minimapWindow = SDL_Rect{};
	if (feed.inventoryGrid) feed.inventoryGrid->setSelectedItem(nullptr); // (an item picked up goes back where it was)
	feed.inventoryGrid.reset();
	feed.inventoryUnit = nullptr;
	feed.inventoryKey.clear();
	feed.maximised = CommandFeed::MAX_NONE;
	feed.width = 0; // (laid out again when it comes back)
	feed.moving = -1;
	feed.gliding = false;
	feed.pressedOnDrone = feed.overDrone = false;
	feed.on = false;
}

/**
 * Lays the feed out for the screen (in the game's screen pixels), again only when the screen, the top bar's foot or
 * the squad's count changed: under the top bar, down to FeedUpperShare of the screen, the inventory window at the left
 * and the 3D view's window; below, the drone's window at the left and the minimap's at the right (if there is room),
 * and the drone's picture all over the screen. The minimap is made for its window.
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
	const int ui = feed.ui, border = FeedBorder * ui, titleH = FeedTitleH * ui, gap = FeedGap * ui;

	// The inventory window and the 3D view's, under the top bar down to FeedUpperShare of the screen: the inventory as
	// wide as its screen's picture shows whole at the height there (at most FeedInventoryShare of the width).
	const int upperTop = top + gap;
	const int upperBottom = std::max(upperTop + 4 * titleH, (int)(height * FeedUpperShare));
	const int pictureH = upperBottom - upperTop - 2 * border - titleH;
	const int inventoryW = std::min((int)(width * FeedInventoryShare), pictureH * FeedInventoryScreenW / FeedInventoryScreenH + 2 * border);
	feed.inventory = makeRect(0, upperTop, inventoryW, upperBottom - upperTop);
	feed.inventoryPicture = makeRect(border, upperTop + border + titleH, inventoryW - 2 * border, pictureH);
	feed.view = makeRect(inventoryW, upperTop, width - inventoryW, upperBottom - upperTop);
	feed.viewPicture = makeRect(inventoryW + border, upperTop + border + titleH, width - inventoryW - 2 * border, pictureH);
	// (the inventory's screen scaled to fit its window whole, in the middle; the source pixel of each shown one)
	const double scale = std::min(feed.inventoryPicture.w / (double)FeedInventoryScreenW, feed.inventoryPicture.h / (double)FeedInventoryScreenH);
	const int shownW = std::max(1, (int)(FeedInventoryScreenW * scale)), shownH = std::max(1, (int)(FeedInventoryScreenH * scale));
	feed.inventoryShown = makeRect(feed.inventoryPicture.x + (feed.inventoryPicture.w - shownW) / 2, feed.inventoryPicture.y + (feed.inventoryPicture.h - shownH) / 2, shownW, shownH);
	feed.inventoryColumns.resize(shownW);
	feed.inventoryRows.resize(shownH);
	for (int column = 0; column < shownW; ++column) feed.inventoryColumns[column] = std::min(FeedInventoryScreenW - 1, (int)(column / scale));
	for (int row = 0; row < shownH; ++row) feed.inventoryRows[row] = std::min(FeedInventoryScreenH - 1, (int)(row / scale));

	// The drone's window and the minimap's: the drone's at least as wide as the game's panel under it.
	const int middle = upperBottom + gap;
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
 * The feed's buttons for this frame, in the windows' title lines (filled from the right): the drone's FOLLOW, FLY TO,
 * stabiliser, night vision (realistic, then enhanced), thermal, names, boxes and all over the screen, and on its picture
 * the arrows at its edges (held: it flies that way) and the level buttons at its right; the minimap's all over the screen; the 3D window's, which goes to the full 3D
 * view; the inventory window's, which opens the game's inventory screen.
 */
void FirstPersonMode::feedButtons()
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, border = FeedBorder * ui, titleH = FeedTitleH * ui, inset = FeedButtonInset * ui, side = titleH - 2 * inset;
	feed.buttons.clear();
	// (a button at the right of a title line, left of the last one: its words' width, or a square for a sign)
	auto add = [&](CommandFeed::ButtonKind kind, const SDL_Rect &frame, int &right, const std::string &words, bool lit) {
		int wide = side;
		if (!words.empty())
		{
			feed.writer->setText(words);
			wide = std::max(side, feed.writer->getTextWidth() * ui + 2 * FeedButtonPad * ui);
		}
		right -= wide + inset;
		feed.buttons.push_back({ kind, makeRect(right, frame.y + border + inset, wide, side), words, 0, lit });
	};
	const SDL_Rect &drone = feed.droneFrame();
	int right = drone.x + drone.w - border;
	add(CommandFeed::FB_DRONE_MAX, drone, right, "", feed.maximised == CommandFeed::MAX_DRONE);
	add(CommandFeed::FB_BOXES, drone, right, _state->tr("STR_FPS_FEED_BOXES"), feed.boxes);
	add(CommandFeed::FB_NAMES, drone, right, _state->tr("STR_FPS_FEED_NAMES"), feed.names);
	add(CommandFeed::FB_THERMAL, drone, right, _state->tr("STR_FPS_FEED_THERMAL"), feed.vision == CommandFeed::VISION_THERMAL);
	add(CommandFeed::FB_NIGHT, drone, right, _state->tr(feed.vision == CommandFeed::VISION_NIGHT_PLUS ? "STR_FPS_FEED_NIGHT_PLUS" : "STR_FPS_FEED_NIGHT"),
		feed.vision == CommandFeed::VISION_NIGHT || feed.vision == CommandFeed::VISION_NIGHT_PLUS);
	add(CommandFeed::FB_STABILISER, drone, right, _state->tr("STR_FPS_FEED_STABILISER"), feed.stabiliser);
	add(CommandFeed::FB_FLY_TO, drone, right, _state->tr("STR_FPS_FEED_FLY_TO"), feed.gliding);
	add(CommandFeed::FB_FOLLOW, drone, right, _state->tr("STR_FPS_FEED_FOLLOW"), feed.follow);
	// (the arrows at the drone picture's edges, and the level buttons at its right under the top one)
	const SDL_Rect &picture = feed.dronePictureNow();
	const int along = FeedArrowLong * ui, deep = FeedArrowDeep * ui, arrowInset = FeedArrowInset * ui, levelSide = FeedLevelButton * ui;
	const int middleX = picture.x + picture.w / 2, middleY = picture.y + picture.h / 2;
	feed.buttons.push_back({ CommandFeed::FB_MOVE_UP, makeRect(middleX - along / 2, picture.y + arrowInset, along, deep), std::string(), 0, false });
	feed.buttons.push_back({ CommandFeed::FB_MOVE_DOWN, makeRect(middleX - along / 2, picture.y + picture.h - arrowInset - deep, along, deep), std::string(), 1, false });
	feed.buttons.push_back({ CommandFeed::FB_MOVE_LEFT, makeRect(picture.x + arrowInset, middleY - along / 2, deep, along), std::string(), 2, false });
	feed.buttons.push_back({ CommandFeed::FB_MOVE_RIGHT, makeRect(picture.x + picture.w - arrowInset - deep, middleY - along / 2, deep, along), std::string(), 3, false });
	const int levelX = picture.x + picture.w - arrowInset - levelSide, levelY = picture.y + arrowInset + deep + 4 * ui;
	feed.buttons.push_back({ CommandFeed::FB_LEVEL_UP, makeRect(levelX, levelY, levelSide, levelSide), std::string(), 0, true });
	feed.buttons.push_back({ CommandFeed::FB_LEVEL_DOWN, makeRect(levelX, levelY + levelSide + FeedWordsH * ui + 2 * ui, levelSide, levelSide), std::string(), 1, true });
	if (feed.maximised == CommandFeed::MAX_DRONE)
	{
		return;
	}
	if (feed.map.w > 0)
	{
		int mapRight = feed.map.x + feed.map.w - border;
		add(CommandFeed::FB_MAP_MAX, feed.map, mapRight, "", false);
	}
	int viewRight = feed.view.x + feed.view.w - border;
	add(CommandFeed::FB_VIEW_MAX, feed.view, viewRight, "", false);
	int inventoryRight = feed.inventory.x + feed.inventory.w - border;
	add(CommandFeed::FB_INVENTORY, feed.inventory, inventoryRight, "", false);
}

/**
 * Puts the battle's screen right for the feed: the game's panel hidden (the classic HUD: the top bar and the windows
 * stand for it), its warnings at the foot of the drone's picture, and the map's camera told to show its middle in the
 * drone's picture (see Camera::setViewWindow, the icons no longer at its foot): what it looked at stays in the middle of
 * what shows.
 */
void FirstPersonMode::placeFeedScreen()
{
	CommandFeed &feed = *_feed;
	hideGamePanel(true);
	const SDL_Rect &picture = feed.dronePictureNow();
	WarningMessage *warning = _state->_warning;
	warning->setX(picture.x + (picture.w - warning->getWidth()) / 2);
	warning->setY(picture.y + picture.h - warning->getHeight() - 4 * feed.ui);
	if (!sameRect(picture, feed.cameraWindow))
	{
		Camera *camera = _state->_map->getCamera();
		const Position centre = camera->getCenterPosition();
		camera->setViewWindow(picture.x, picture.y, picture.w, picture.h, false);
		camera->centerOnPosition(centre, false);
		feed.cameraWindow = picture;
	}
}

/**
 * Draws the soldier's figure as the game's inventory screen draws it (see InventoryState::init): a soldier's armour by
 * their look (its layers, or its picture for their gender and look), anyone else's armour's picture.
 * @param unit The soldier.
 */
void FirstPersonMode::drawFeedDoll(BattleUnit *unit)
{
	CommandFeed &feed = *_feed;
	feed.doll->clear();
	Mod *mod = _game->getMod();
	const Soldier *soldier = unit->getGeoscapeSoldier();
	if (soldier && soldier->getArmor()->hasLayersDefinition())
	{
		for (const auto &layer : soldier->getArmorLayers())
		{
			if (Surface *picture = mod->getSurface(layer, false)) picture->blitNShade(feed.doll.get(), 0, 0);
		}
		return;
	}
	Surface *picture = nullptr;
	if (soldier)
	{
		const std::string look = soldier->getArmor()->getSpriteInventory(), gender = soldier->getGender() == GENDER_MALE ? "M" : "F";
		for (int bit = 0; bit <= RuleSoldier::LookVariantBits && !picture; ++bit)
		{
			std::ostringstream name;
			name << look << gender << (int)soldier->getLook() + (soldier->getLookVariant() & (RuleSoldier::LookVariantMask >> bit)) * 4 << ".SPK";
			picture = mod->getSurface(name.str(), false);
		}
		if (!picture) picture = mod->getSurface(look + ".SPK", false);
		if (!picture) picture = mod->getSurface(look, false);
	}
	else
	{
		const std::string look = unit->getArmor()->getSpriteInventory();
		picture = mod->getSurface(look, false);
		if (!picture) picture = mod->getSurface(look + ".SPK", false);
		if (!picture) picture = mod->getSurface(look + "M0.SPK", false);
	}
	if (picture) picture->blitNShade(feed.doll.get(), 0, 0);
}

/**
 * Keeps the inventory window's inventory on the soldier: the game's own inventory (made the first time, in the battle's
 * time unit mode: moving an item costs what it costs on its screen), set on the soldier when another is selected (with
 * their figure), and arranged again when their items or the ground under them changed (not while an item is held).
 * Then the screen's picture: its background, the figure, the inventory over them.
 * @param unit The soldier (null: none to show).
 */
void FirstPersonMode::updateFeedInventory(BattleUnit *unit)
{
	CommandFeed &feed = *_feed;
	if (!feed.inventoryGrid)
	{
		feed.inventoryGrid.reset(new Inventory(_game, FeedInventoryScreenW, FeedInventoryScreenH, 0, 0, false));
		feed.inventoryGrid->setPalette(_state->getPalette());
		feed.inventoryGrid->setTuMode(true);
		feed.inventoryGrid->draw();
		feed.inventoryScreen.reset(new Surface(FeedInventoryScreenW, FeedInventoryScreenH, 0, 0));
		feed.doll.reset(new Surface(FeedInventoryScreenW, FeedInventoryScreenH, 0, 0));
		feed.inventoryScreen->setPalette(_state->getPalette());
		feed.doll->setPalette(_state->getPalette());
		feed.inventoryUnit = nullptr;
		feed.inventoryKey.clear();
	}
	Inventory &grid = *feed.inventoryGrid;
	if (unit != feed.inventoryUnit)
	{
		grid.setSelectedItem(nullptr); // (an item held goes back where it was)
		feed.inventoryUnit = unit;
		feed.inventoryKey.clear();
		if (unit)
		{
			grid.setSelectedUnit(unit, true);
			drawFeedDoll(unit);
		}
	}
	if (unit && !grid.getSelectedItem())
	{
		// (what the inventory shows: the soldier's items in their places and their rounds, the ground's items)
		std::ostringstream key;
		const Position at = unit->getPosition();
		key << at.x << ',' << at.y << ',' << at.z;
		for (const BattleItem *item : *unit->getInventory())
		{
			const BattleItem *ammo = item->getAmmoForSlot(0);
			key << ' ' << item->getId() << ':' << (item->getSlot() ? item->getSlot()->getId() : std::string()) << ':' << item->getSlotX() << ':' << item->getSlotY()
				<< ':' << (ammo ? ammo->getAmmoQuantity() : item->getAmmoQuantity());
		}
		if (Tile *ground = unit->getTile())
		{
			for (const BattleItem *item : *ground->getInventory()) key << " g" << item->getId();
		}
		if (key.str() != feed.inventoryKey)
		{
			const bool first = feed.inventoryKey.empty();
			feed.inventoryKey = key.str();
			grid.setSelectedUnit(unit, false);
			if (!first) _state->updateSoldierInfo(); // (moved in the window: the panel's hands and time units again)
		}
	}
	grid.think();

	// The screen's picture: its background, the figure, the inventory (its grid, items, the item held, its warnings).
	Surface &screen = *feed.inventoryScreen;
	screen.clear();
	if (Surface *background = _game->getMod()->getSurface("TAC01.SCR", false)) background->blitNShade(&screen, 0, 0);
	if (unit)
	{
		feed.doll->blitNShade(&screen, 0, 0);
		grid.blit(screen.getSurface());
	}
}

/**
 * Hands a mouse event in the inventory window to the inventory, at the point of its screen's picture under it (the
 * picture is shown scaled): its clicks pick an item up and put it down as on its screen, and the buttons its background
 * draws work as there (OK aside: the window stays). Only in the player's turn, with the game taking orders; else
 * nothing.
 * @param action The mouse's event.
 * @param x, y Where it is on the game's screen.
 */
void FirstPersonMode::feedInventoryMouse(Action *action, int x, int y)
{
	CommandFeed &feed = *_feed;
	if (!feed.inventoryGrid || !feed.inventoryUnit || !canAct() || !inside(feed.inventoryShown, x, y) || feed.inventoryColumns.empty())
	{
		return;
	}
	const int pictureX = feed.inventoryColumns[x - feed.inventoryShown.x], pictureY = feed.inventoryRows[y - feed.inventoryShown.y];
	// The buttons the background draws, as on the inventory's screen (a left press): the soldier before or the next
	// (selected for the 3D window too), unload the weapon held, more of the ground (Shift: back).
	const SDL_Event &details = *action->getDetails();
	if (details.type == SDL_MOUSEBUTTONDOWN && details.button.button == SDL_BUTTON_LEFT)
	{
		auto on = [&](const int box[4]) { return pictureX >= box[0] && pictureY >= box[1] && pictureX < box[0] + box[2] && pictureY < box[1] + box[3]; };
		if (on(FeedInventoryPrevious) || on(FeedInventoryNext))
		{
			feed.inventoryGrid->setSelectedItem(nullptr);
			if (BattleUnit *other = on(FeedInventoryNext) ? _save->selectNextPlayerUnit(false, false, true) : _save->selectPreviousPlayerUnit(false, false, true))
			{
				selectUnit(other);
			}
			return;
		}
		if (on(FeedInventoryUnload))
		{
			if (feed.inventoryGrid->unload(false)) _game->getMod()->getSoundByDepth(0, Mod::ITEM_DROP)->play();
			return;
		}
		if (on(FeedInventoryGround))
		{
			feed.inventoryGrid->arrangeGround(_game->isShiftPressed() ? -1 : 1);
			return;
		}
	}
	SDL_Event event = details;
	const Uint16 eventX = (Uint16)std::lround(pictureX * action->getXScale() + action->getLeftBlackBand());
	const Uint16 eventY = (Uint16)std::lround(pictureY * action->getYScale() + action->getTopBlackBand());
	if (event.type == SDL_MOUSEMOTION)
	{
		event.motion.x = eventX;
		event.motion.y = eventY;
	}
	else
	{
		event.button.x = eventX;
		event.button.y = eventY;
	}
	Action scaled(&event, action->getXScale(), action->getYScale(), action->getTopBlackBand(), action->getLeftBlackBand());
	feed.inventoryGrid->handle(&scaled, _state);
}

/**
 * Does the battle's blit (see blit) draw this surface of the battle's own with the feed showing? Only its warnings: the
 * map is the drone's, the panel is hidden (see hideGamePanel), the edges' are drawn after the windows, over them.
 * @param surface The surface.
 * @return True if the blit draws it.
 */
bool FirstPersonMode::feedDrawsGameSurface(const Surface *surface) const
{
	return surface == _state->_warning;
}

/**
 * Which of our own overlays show with the feed (see drawOverlays): the top bar with the squad's buttons, the soldier
 * card a squad button shows (with its helmet camera) and the soldier cam, and in the 3D window, while the 3D view plays
 * there, the crosshair and the words beside it (the shot, its odds and cost, the range, the unit pointed at): the rest
 * of the 3D HUD the windows show.
 * @param surface Our surface.
 * @return True if it shows.
 */
bool FirstPersonMode::feedOverlay(const Surface *surface) const
{
	Surface *own = const_cast<Surface*>(surface);
	if (_topBarSurfaces.count(own))
	{
		return true;
	}
	if (own == _card || own == _preview || own == _cardName || own == _cardWeapon || own == _cardStatus || own == _cardSignal || own == _cardRank || own == _cardIcon
		|| own == _soldierCam || own == _soldierCamFrame || own == _soldierCamDot || own == _txtSoldierCam || own == _soldierCamRank)
	{
		return true;
	}
	for (int stat = 0; stat < 4; ++stat)
	{
		if (own == _cardNumbers[stat] || own == _cardBars[stat]) return true;
	}
	const bool viewPlays = isActive() && _feed->maximised == CommandFeed::MAX_NONE;
	return viewPlays && (own == _crosshair || own == _txtMode || own == _txtShot || own == _txtDamage || own == _txtRange || own == _txtAimOff
		|| own == _txtTarget || own == _txtCosts);
}

/**
 * Draws what of the feed goes under its windows: the map in the drone's picture (as the game drew it: only that part
 * of it shows), floating a little with the stabiliser off (a slow drift of a few pixels, as a hovering camera's; where it
 * uncovers the picture's edge past the map, the map's edge goes on), turned into the drone's feed (see filterDrone),
 * and the marks over the soldiers.
 * @param screen The game's screen.
 */
void FirstPersonMode::blitFeedUnder(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	SDL_Rect picture = feed.dronePictureNow();
	feed.swayX = feed.swayY = 0;
	if (!feed.stabiliser)
	{
		const double now = SDL_GetTicks(), most = FeedSwayMost * feed.ui, turn = 6.283185307179586;
		feed.swayX = (int)std::lround(most * (0.65 * std::sin(turn * now / FeedSwayPeriods[0]) + 0.35 * std::sin(turn * now / FeedSwayPeriods[1] + 0.7)));
		feed.swayY = (int)std::lround(most * (0.65 * std::sin(turn * now / FeedSwayPeriods[2] + 1.3) + 0.35 * std::sin(turn * now / FeedSwayPeriods[3] + 0.4)));
	}
	fill(screen, picture.x, picture.y, picture.w, picture.h, feed.black);
	SDL_SetClipRect(screen, &picture);
	Map *map = _state->_map;
	if (map->showsHiddenMovement())
	{
		// (the aliens' turn with nothing of it seen: the drone's own screen, not the map's box)
		for (int speck = 0; speck < picture.w * picture.h / FeedSnowEvery; ++speck)
		{
			const Uint32 dice = feed.random();
			fill(screen, picture.x + (int)(dice % (Uint32)std::max(1, (int)picture.w)), picture.y + (int)((dice >> 12) % (Uint32)std::max(1, (int)picture.h)), feed.ui, feed.ui, feed.snow[(dice >> 28) & 7]);
		}
		const std::string words = _state->tr("STR_HIDDEN_MOVEMENT");
		feed.writer->setText(words);
		feedWrite(screen, words, picture.x + (picture.w - feed.writer->getTextWidth() * feed.ui) / 2, picture.y + (picture.h - FeedWordsH * feed.ui) / 2, feed.wordsColour, 0, true);
		SDL_SetClipRect(screen, nullptr);
		return;
	}
	map->setX(feed.swayX);
	map->setY(feed.swayY);
	map->blit(screen);
	map->setX(0);
	map->setY(0);
	// (the map's edge goes on where the drift left the picture without it)
	const int left = std::max(0, (int)picture.x), top = std::max(0, (int)picture.y);
	const int right = std::min(screen->w, picture.x + (int)picture.w), bottom = std::min(screen->h, picture.y + (int)picture.h);
	const int fromX = std::clamp(feed.swayX, left, right - 1), toX = std::clamp(feed.swayX + map->getWidth(), left + 1, right);
	const int fromY = std::clamp(feed.swayY, top, bottom - 1), toY = std::clamp(feed.swayY + map->getHeight(), top + 1, bottom);
	for (int y = fromY; y < toY && (fromX > left || toX < right); ++y)
	{
		Uint8 *row = (Uint8 *)screen->pixels + (size_t)y * screen->pitch;
		for (int x = left; x < fromX; ++x) row[x] = row[fromX];
		for (int x = toX; x < right; ++x) row[x] = row[toX - 1];
	}
	for (int y = top; y < fromY; ++y) std::memcpy((Uint8 *)screen->pixels + (size_t)y * screen->pitch + left, (Uint8 *)screen->pixels + (size_t)fromY * screen->pitch + left, (size_t)(right - left));
	for (int y = toY; y < bottom; ++y) std::memcpy((Uint8 *)screen->pixels + (size_t)y * screen->pitch + left, (Uint8 *)screen->pixels + (size_t)(toY - 1) * screen->pitch + left, (size_t)(right - left));
	filterDrone(screen, picture);
	if (feed.names || feed.boxes)
	{
		markSoldiers(screen, picture);
	}
	SDL_SetClipRect(screen, nullptr);
}

/**
 * A window's frame: its border and its title line's ground.
 * @param screen The game's screen.
 * @param frame The window.
 * @param borderColour The border's colour.
 */
void FirstPersonMode::drawFeedWindow(SDL_Surface *screen, const SDL_Rect &frame, Uint8 borderColour)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, border = FeedBorder * ui;
	outline(screen, frame, border, borderColour);
	fill(screen, frame.x + border, frame.y + border, frame.w - 2 * border, FeedTitleH * ui, feed.titleBack);
}

/**
 * The whole screen with the feed (see blit): the drone's picture with the game's own surfaces over it (its panel),
 * then the feed's windows over those, then our top bar and the 3D window's crosshair.
 * @param screen The game's screen.
 */
void FirstPersonMode::blitFeed(SDL_Surface *screen)
{
	blitFeedUnder(screen);
	for (Surface *surface : _state->_surfaces)
	{
		if (!_ownSurfaces.count(surface) && feedDrawsGameSurface(surface)) surface->blit(screen);
	}
	blitFeedOver(screen);
	_feedOverlays = true;
	drawOverlays(screen, false, false);
	_feedOverlays = false;
}

/**
 * Draws the feed's windows over the battle's screen: the inventory window; the 3D window (titled with the soldier looked through, a red dot that blinks, and whether the mouse plays there: the 3D view
 * itself, drawn at the window's size, or "NO SIGNAL" while it isn't up); the drone's frame and title (the turn and the
 * time); the minimap; or the drone's picture all over the screen. Then the battle's buttons at the screen's edges,
 * over all of it.
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

	if (feed.maximised == CommandFeed::MAX_NONE)
	{
		// The inventory window: the inventory's screen scaled into it, titled with the soldier's time units.
		drawFeedWindow(screen, feed.inventory, feed.frameColour);
		const BattleUnit *owner = feed.inventoryUnit;
		titleWords(feed.inventory, owner ? std::string(_state->tr("STR_FPS_FEED_INVENTORY_TU").arg(owner->getTimeUnits())) : std::string(_state->tr("STR_FPS_FEED_INVENTORY")),
			feed.wordsColour, false);
		const SDL_Rect &area = feed.inventoryPicture;
		fill(screen, area.x, area.y, area.w, area.h, feed.black);
		if (owner && feed.inventoryScreen)
		{
			const SDL_Surface *source = feed.inventoryScreen->getSurface();
			const SDL_Rect &shown = feed.inventoryShown;
			for (int row = 0; row < (int)shown.h; ++row)
			{
				const int targetY = shown.y + row;
				if (targetY < 0 || targetY >= screen->h) continue;
				const Uint8 *from = (const Uint8 *)source->pixels + (size_t)feed.inventoryRows[row] * source->pitch;
				Uint8 *to = (Uint8 *)screen->pixels + (size_t)targetY * screen->pitch + shown.x;
				for (int column = 0; column < (int)shown.w && shown.x + column < screen->w; ++column) to[column] = from[feed.inventoryColumns[column]];
			}
		}
		else
		{
			const std::string words = _state->tr("STR_FPS_FEED_NO_SOLDIER");
			feed.writer->setText(words);
			feedWrite(screen, words, area.x + (area.w - feed.writer->getTextWidth() * ui) / 2, area.y + (area.h - FeedWordsH * ui) / 2, feed.dimColour);
		}

		// The 3D window: the 3D view at the window's size, or no signal.
		const bool plays = isActive() && _view->getVisible();
		drawFeedWindow(screen, feed.view, plays && wantsMouseCapture() ? feed.frameLit : feed.frameColour);
		const BattleUnit *looked = plays && _trackedUnit ? _trackedUnit : _save->getSelectedUnit();
		std::string title = looked ? looked->getName(_game->getLanguage(), false) : std::string();
		for (const auto &member : feedSquad()) if (member.first == looked) title = std::to_string(member.second) + " " + title;
		const int after = titleWords(feed.view, title + "  " + std::string(_state->tr("STR_FPS_FEED_LIVE")), feed.wordsColour, true);
		if (plays)
		{
			// (how the mouse is handed over: a click to play, the mouse mode's key or Escape to let go)
			const std::string hint = wantsMouseCapture() ? std::string(_state->tr("STR_FPS_FEED_LET_GO")).append(" ").append(keyLabel(Options::keyFpsMouseMode))
				: std::string(_state->tr("STR_FPS_FEED_CLICK_TO_PLAY"));
			int hintRoom = feed.view.x + feed.view.w - border - after - 6 * ui;
			for (const CommandFeed::Button &button : feed.buttons) if (button.kind == CommandFeed::FB_VIEW_MAX) hintRoom = button.box.x - after - 6 * ui;
			feed.writer->setText(hint);
			const int hintW = std::min(hintRoom, feed.writer->getTextWidth() * ui);
			if (hintW > 0) feedWrite(screen, hint, after + 4 * ui + hintRoom - hintW, feed.view.y + wordsY, wantsMouseCapture() ? feed.hudColour : feed.pickedColour, hintRoom);
			_view->drawIfAsked();
			SDL_Rect where = feed.viewPicture;
			SDL_BlitSurface(_view->getSurface(), nullptr, screen, &where);
		}
		else
		{
			const SDL_Rect &picture = feed.viewPicture;
			fill(screen, picture.x, picture.y, picture.w, picture.h, feed.black);
			for (int speck = 0; speck < picture.w * picture.h / FeedSnowEvery; ++speck)
			{
				const Uint32 dice = feed.random();
				fill(screen, picture.x + (int)(dice % (Uint32)std::max(1, (int)picture.w)), picture.y + (int)((dice >> 12) % (Uint32)std::max(1, (int)picture.h)), ui, ui, feed.snow[(dice >> 28) & 7]);
			}
			const std::string words = _state->tr("STR_FPS_FEED_NO_SIGNAL");
			feed.writer->setText(words);
			feedWrite(screen, words, picture.x + (picture.w - feed.writer->getTextWidth() * ui) / 2, picture.y + (picture.h - FeedWordsH * ui) / 2, feed.wordsColour, 0, true);
		}
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

	// The title lines' buttons: their words or signs (all over the screen: a square; back: a square in a square), lit
	// when on.
	for (const CommandFeed::Button &button : feed.buttons)
	{
		if (feed.maximised != CommandFeed::MAX_NONE && !droneButton(button.kind)) continue;
		if (moveArrow(button.kind))
		{
			// (discreet: a triangle alone, lit under the mouse or held)
			const bool over = inside(button.box, feed.mouseX, feed.mouseY) || feed.moving == button.kind;
			triangle(screen, button.box, button.value, over ? feed.frameLit : feed.frameColour);
			continue;
		}
		if (button.kind == CommandFeed::FB_LEVEL_UP || button.kind == CommandFeed::FB_LEVEL_DOWN)
		{
			// (purple, as the isometric view's level buttons stand out; the level looked at between them)
			const bool over = inside(button.box, feed.mouseX, feed.mouseY);
			fill(screen, button.box.x, button.box.y, button.box.w, button.box.h, feed.titleBack);
			outline(screen, button.box, ui, over ? feed.purpleLit : feed.purple);
			const int margin = 3 * ui;
			triangle(screen, makeRect(button.box.x + margin, button.box.y + margin, button.box.w - 2 * margin, button.box.h - 2 * margin), button.value, over ? feed.purpleLit : feed.purple);
			if (button.kind == CommandFeed::FB_LEVEL_UP)
			{
				const std::string level = std::to_string(_state->_map->getCamera()->getViewLevel() + 1);
				feed.writer->setText(level);
				feedWrite(screen, level, button.box.x + (button.box.w - feed.writer->getTextWidth() * ui) / 2, button.box.y + button.box.h + ui, feed.purpleLit, 0, true);
			}
			continue;
		}
		const Uint8 colour = button.lit ? feed.frameLit : feed.frameColour;
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
			if (button.kind == CommandFeed::FB_DRONE_MAX && button.lit)
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
 * A key while the feed shows: Escape puts the drone's picture all over the screen back, or lets go of the mouse the 3D
 * window has (both its press and its release are then the feed's). The rest are the battle's and the 3D view's.
 * @param action The key.
 * @return True if it was the feed's.
 */
bool FirstPersonMode::handleFeedKey(Action *action)
{
	if (!feedShown() || action->getDetails()->key.keysym.sym != Options::keyCancel)
	{
		return false;
	}
	const bool down = action->getDetails()->type == SDL_KEYDOWN;
	if (_feed->maximised != CommandFeed::MAX_NONE)
	{
		if (down) _feed->maximised = CommandFeed::MAX_NONE;
		return true;
	}
	if (isActive() && _mouseLook)
	{
		if (down) setMouseLook(false);
		return true;
	}
	return false;
}

/**
 * The mouse while the feed shows and the 3D window doesn't have it (its moves, buttons and wheel), after our top bar's
 * buttons had theirs:
 *  - a press on the drone's picture is the map's until the button comes up (a drag scrolls on past the window);
 *  - the battle's buttons at the screen's edges, theirs;
 *  - the feed's buttons (in the windows' title lines): a left press works one;
 *  - the minimap: its own (all the tactical map's buttons, its drag and wheel);
 *  - the inventory window: the inventory's (items picked up and put down, in the player's turn);
 *  - the 3D window: a left click gives it the mouse (the soldier plays there as in the full 3D view);
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
	const int wheel = wheelStep(event);
	const bool press = event.type == SDL_MOUSEBUTTONDOWN && wheel == 0, leftPress = press && event.button.button == SDL_BUTTON_LEFT;

	// (an event the map gets: with the drone floating, at the map's own point under the pointer)
	auto toMap = [&]() {
		if (!feed.swayX && !feed.swayY) return;
		SDL_Event *details = action->getDetails();
		const int byX = (int)std::lround(feed.swayX * action->getXScale()), byY = (int)std::lround(feed.swayY * action->getYScale());
		if (details->type == SDL_MOUSEMOTION)
		{
			details->motion.x = (decltype(details->motion.x))std::max(0, (int)details->motion.x - byX);
			details->motion.y = (decltype(details->motion.y))std::max(0, (int)details->motion.y - byY);
		}
		else if (details->type == SDL_MOUSEBUTTONDOWN || details->type == SDL_MOUSEBUTTONUP)
		{
			details->button.x = (decltype(details->button.x))std::max(0, (int)details->button.x - byX);
			details->button.y = (decltype(details->button.y))std::max(0, (int)details->button.y - byY);
		}
	};
	feed.mouseX = x;
	feed.mouseY = y;
	if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
	{
		feed.moving = -1; // (an arrow let go)
	}
	if (feed.pressedOnDrone)
	{
		if (event.type == SDL_MOUSEBUTTONUP) feed.pressedOnDrone = false;
		toMap();
		return false;
	}
	const bool onDrone = inside(feed.dronePictureNow(), x, y);
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
		const CommandFeed::Button &each = feed.buttons[button];
		const bool shown = feed.maximised == CommandFeed::MAX_NONE || droneButton(each.kind);
		if (shown && inside(each.box, x, y))
		{
			if (leftPress) pressFeedButton((int)button, action);
			return true;
		}
	}
	if (feed.maximised == CommandFeed::MAX_NONE)
	{
		if (feed.minimap && inside(feed.mapPicture, x, y))
		{
			feed.minimap->handle(action);
			return true;
		}
		if (inside(feed.inventory, x, y))
		{
			if (inside(feed.inventoryPicture, x, y) && wheel == 0) feedInventoryMouse(action, x, y);
			return true;
		}
		if (inside(feed.view, x, y))
		{
			if (leftPress && inside(feed.viewPicture, x, y) && isActive())
			{
				setMouseLook(true); // (the soldier plays in the window: the mouse turns the view, the buttons fire)
			}
			return true;
		}
	}
	if (onDrone)
	{
		if (press) feed.pressedOnDrone = true;
		toMap();
		return false;
	}
	return feedWithoutMap(action);
}

/**
 * FLY TO: the drone glides to the soldier looked through (the selected one; in the aliens' turn the one the 3D window
 * shows), on their level at once and across in FeedFlyMs, smoothly.
 */
void FirstPersonMode::flyFeedTo()
{
	CommandFeed &feed = *_feed;
	const BattleUnit *unit = isActive() && _trackedUnit ? _trackedUnit : _save->getSelectedUnit();
	if (!unit || unit->isOut() || !unit->getTile())
	{
		return;
	}
	Camera *camera = _state->_map->getCamera();
	const Position from = camera->getMapOffset();
	camera->centerOnPosition(unit->getPosition(), false);
	const Position to = camera->getMapOffset();
	camera->setMapOffset(Position(from.x, from.y, to.z));
	camera->scrollXY(0, 0, true); // (its middle and the map's bounds again)
	feed.glideFrom = camera->getMapOffset();
	feed.glideTo = to;
	feed.glideStart = SDL_GetTicks();
	feed.gliding = true;
	feed.moving = -1;
}

/**
 * One of the feed's buttons pressed (with the buttons' click).
 * @param index The button's place in the list.
 * @param action The click (the game's inventory button wants it).
 */
void FirstPersonMode::pressFeedButton(int index, Action *action)
{
	CommandFeed &feed = *_feed;
	const CommandFeed::Button button = feed.buttons[index];
	if (TextButton::soundPress && !moveArrow(button.kind)) TextButton::soundPress->play(Mix_GroupAvailable(0)); // (an arrow flies quietly)
	if (button.kind == CommandFeed::FB_NIGHT || button.kind == CommandFeed::FB_THERMAL)
	{
		_state->_map->invalidate(); // (its mask drawn for the new vision, see updateFeed)
	}
	switch (button.kind)
	{
	case CommandFeed::FB_NIGHT:
		// (black and white or thermal, then realistic night vision, then enhanced, then black and white again)
		feed.vision = feed.vision == CommandFeed::VISION_NIGHT ? CommandFeed::VISION_NIGHT_PLUS
			: feed.vision == CommandFeed::VISION_NIGHT_PLUS ? CommandFeed::VISION_PLAIN : CommandFeed::VISION_NIGHT;
		break;
	case CommandFeed::FB_THERMAL: feed.vision = feed.vision == CommandFeed::VISION_THERMAL ? CommandFeed::VISION_PLAIN : CommandFeed::VISION_THERMAL; break;
	case CommandFeed::FB_STABILISER: feed.stabiliser = !feed.stabiliser; break;
	case CommandFeed::FB_FLY_TO: flyFeedTo(); break;
	case CommandFeed::FB_FOLLOW: feed.follow = !feed.follow; break;
	case CommandFeed::FB_MOVE_UP: case CommandFeed::FB_MOVE_DOWN: case CommandFeed::FB_MOVE_LEFT: case CommandFeed::FB_MOVE_RIGHT:
		feed.moving = button.kind; // (flies while held, see updateFeed)
		feed.gliding = false;
		feed.follow = false; // (flown by hand: no longer following)
		return;
	case CommandFeed::FB_LEVEL_UP: _state->_map->getCamera()->up(); break;
	case CommandFeed::FB_LEVEL_DOWN: _state->_map->getCamera()->down(); break;
	case CommandFeed::FB_NAMES: feed.names = !feed.names; break;
	case CommandFeed::FB_BOXES: feed.boxes = !feed.boxes; break;
	case CommandFeed::FB_DRONE_MAX: feed.maximised = feed.maximised == CommandFeed::MAX_DRONE ? CommandFeed::MAX_NONE : CommandFeed::MAX_DRONE; break;
	case CommandFeed::FB_MAP_MAX: showTacticalMap(); break;
	case CommandFeed::FB_VIEW_MAX: toggleView(); return; // (to the full 3D view: the feed is let go of)
	case CommandFeed::FB_INVENTORY:
		if (feed.inventoryGrid) feed.inventoryGrid->setSelectedItem(nullptr); // (an item held goes back first)
		_state->btnInventoryClick(action); // (the game's inventory screen)
		return;
	}
	feedButtons();
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
	// (thermal: a cool world in dim greys, what is hot white)
	for (int colour = 0; colour < 256; ++colour)
	{
		const double light = 0.30 * palette[colour].r + 0.59 * palette[colour].g + 0.11 * palette[colour].b;
		for (int shade = 0; shade < 3; ++shade)
		{
			const int cool = std::clamp((int)std::lround((26.0 + light * 0.42) * shades[shade]), 0, 255);
			const int hot = std::clamp((int)std::lround((205.0 + light * 0.2) * (shade == 1 ? 0.92 : 1.0)), 0, 255);
			feed.cool[shade][colour] = nearestColor(palette, cool, cool, cool);
			feed.hot[shade][colour] = nearestColor(palette, hot, hot, hot);
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
	feed.purple = bright(150, 70, 220);
	feed.purpleLit = bright(215, 140, 255);
	if (!feed.writer)
	{
		feed.writer.reset(new Text(Options::baseXResolution, FeedWordsH, 0, 0));
		feed.writer->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
		feed.writer->setSmall();
	}
	feed.writer->setPalette(palette);
}

/**
 * Hides the game's panel with the feed, or shows it again (the classic HUD: its buttons, bars and words, the enemies'
 * buttons by it, the special actions' at the right): every one of the battle's own surfaces but the map, its warnings,
 * the line of debug words, OXCE's touch buttons and ours. Hidden, they're neither drawn nor clicked. Done again each frame
 * while the feed shows (the game shows its surfaces again after some of its windows).
 * @param hide True to hide them, false to show them again.
 */
void FirstPersonMode::hideGamePanel(bool hide)
{
	CommandFeed &feed = *_feed;
	if (!hide && !feed.panelHidden)
	{
		return;
	}
	if (hide && !feed.panelHidden)
	{
		feed.warningX = _state->_warning->getX();
		_state->_mouseOverIcons = false;
	}
	for (Surface *surface : _state->_surfaces)
	{
		if (surface == _state->_map || surface == _state->_warning || feedEdgeSurface(surface) || _ownSurfaces.count(surface)) continue;
		surface->setHidden(hide);
	}
	feed.panelHidden = hide;
}

/**
 * Is a surface of the battle's one at the screen's edges, which the feed leaves where it is and draws over its windows
 * (OXCE's touch buttons, the line of debug words)?
 * @param surface The surface.
 * @return True if so.
 */
bool FirstPersonMode::feedEdgeSurface(const Surface *surface) const
{
	return surface == _state->_btnCtrl || surface == _state->_btnAlt || surface == _state->_btnShift || surface == _state->_btnRMB || surface == _state->_btnMMB
		|| surface == _state->_txtDebug;
}

/**
 * Turns the drone's picture into a camera's feed: black and white; night vision's green (enhanced: the units and items
 * in their own colours); or thermal, a cool grey world with the units, explosions and fire white hot; every other line a
 * little darker, a brighter band rolling down, specks of snow, and now and then a tear (a few lines pushed aside).
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
	// (how it sees: the plain colours of each; units, items and effects, as the mask has them where the map's picture
	// still shows them (a wall before a unit hides it), white hot in thermal and their own colours in enhanced night vision)
	Uint8 (*shades)[256] = feed.vision == CommandFeed::VISION_THERMAL ? feed.cool
		: feed.vision == CommandFeed::VISION_NIGHT || feed.vision == CommandFeed::VISION_NIGHT_PLUS ? feed.green : feed.grey;
	const bool thermal = feed.vision == CommandFeed::VISION_THERMAL;
	const SDL_Surface *mask = (thermal || feed.vision == CommandFeed::VISION_NIGHT_PLUS) && feed.mask ? feed.mask->getSurface() : nullptr;
	for (int y = top; y < bottom; ++y)
	{
		const int line = y - top, kind = (line >= band && line < band + FeedBandRows) ? 2 : (line % 2 ? 1 : 0);
		const Uint8 *shade = shades[kind], *hot = feed.hot[kind];
		Uint8 *pixel = (Uint8 *)screen->pixels + (size_t)y * screen->pitch + left;
		const Uint8 *maskRow = mask ? (const Uint8 *)mask->pixels + (size_t)std::clamp(y - feed.swayY, 0, mask->h - 1) * mask->pitch : nullptr;
		for (int x = left; x < right; ++x, ++pixel)
		{
			if (maskRow)
			{
				const Uint8 drawn = maskRow[std::clamp(x - feed.swayX, 0, mask->w - 1)];
				if (drawn && drawn == *pixel)
				{
					*pixel = thermal ? hot[*pixel] : *pixel;
					continue;
				}
			}
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
		*pixel = shades[2][feed.snow[(dice >> 28) & 7]];
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
		screenPosition += Position(feed.swayX, feed.swayY, 0); // (the drone floats, see blitFeedUnder)
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

}
