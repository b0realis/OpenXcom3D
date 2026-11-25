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
 * "a list of scrollable cards ... and only one larger selected window ... on top left a smaller window dedicated to
 * equipment of selected soldier ... you could control the soldier in his larger selected window and drone feed and tac
 * map would update in real time", "let it be performant!". A strip of the soldiers' cards along the top; the equipment
 * window and the selected soldier's 3D view under it, where the soldier is played as in the full 3D view (the view made
 * at the window's size: the only 3D view drawn); below, the battle's own map as a drone's feed (black and white with
 * static, or night vision; each soldier's number and name over them, a box round each), with the game's panel under
 * it, and the tactical map beside it. The drone and the tactical map go on as the soldier moves and fires.
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
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/WarningMessage.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/Options.h"
#include "../Engine/Sound.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Unicode.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
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
/// The arrows at the ends of the strip of cards are this wide; a card is this big.
const int FeedArrowW = 12, FeedCardW = 96, FeedCardH = 30;
/// The equipment window and the 3D view's reach this share of the screen's height down.
const double FeedUpperShare = 0.55;
/// An item's inventory picture is this big (BIGOBS.PCK).
const int FeedItemW = 32, FeedItemH = 48;
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
 * The Live command feed's state (see FirstPersonMode::updateFeed): where its windows are, the cards' soldiers, the
 * equipment window's picture, the minimap, the drone's switches, what of the battle's screen it changed, and its colours.
 */
struct CommandFeed
{
	/// Which window is all over the screen (the 3D view's: the full 3D view, out of the feed).
	enum Maximised { MAX_NONE, MAX_DRONE };
	/// A button: what it does, its place on the screen, its words (none: a drawn sign), its value (a hand, a fire mode),
	/// and whether it shows lit (a switch that is on, the hand or mode in use).
	enum ButtonKind { FB_NIGHT, FB_NAMES, FB_BOXES, FB_DRONE_MAX, FB_MAP_MAX, FB_VIEW_MAX, FB_LEFT, FB_RIGHT, FB_HAND, FB_MODE, FB_INVENTORY };
	struct Button
	{
		ButtonKind kind;
		SDL_Rect box;
		std::string words;
		int value;
		bool lit;
	};
	/// A soldier's card in the strip: whom, their number in the squad, where.
	struct Card
	{
		BattleUnit *unit;
		int number;
		SDL_Rect box;
	};

	// The layout and what it was made for (the screen's size, the top bar's foot, the squad's count).
	int width = 0, height = 0, top = -1, soldiers = -1, ui = 1;
	SDL_Rect strip{}, equipment{}, equipmentPicture{}, view{}, viewPicture{}, drone{}, dronePicture{}, map{}, mapPicture{}, bigFrame{}, bigPicture{};
	int cardsShown = 0, cardsLeft = 0, first = 0;
	std::vector<Card> cards;
	std::vector<Button> buttons, equipmentButtons;
	const BattleUnit *selected = nullptr, *alerted = nullptr, *followed = nullptr;
	Maximised maximised = MAX_NONE;
	// The minimap: the tactical map in its window (made again only for another window).
	std::unique_ptr<FirstPersonMapState> minimap;
	SDL_Rect minimapWindow{};
	// The equipment window's picture, drawn again only when what it shows changed (its key), and a frame for an item's
	// inventory picture.
	std::unique_ptr<Surface> equipmentSurface, itemPicture;
	std::string equipmentKey;
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
	// (plain, a scan line's darker and the rolling band's brighter), the snow, the windows' own and the cards' bars.
	SDL_Color palette[256];
	bool paletteKnown = false;
	Uint8 grey[3][256], green[3][256], snow[8];
	Uint8 black = 0, frameColour = 0, frameLit = 0, titleBack = 0, wordsColour = 0, dimColour = 0, liveColour = 0, hudColour = 0, pickedColour = 0;
	Uint8 timeColour = 0, healthColour = 0, energyColour = 0, barBack = 0;
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
 * Does the Live command feed take the screen? With its setting on, unless the player went to the full 3D view (the 3D
 * key, or the 3D window's button: see toggleView); never in VR mode, whose headset shows the 3D view.
 * @return True if so.
 */
bool FirstPersonMode::feedWanted() const
{
	return Options::fpsCommandFeed && _feedPreferred && !vrOn();
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
 * Each frame (see think): the Live command feed, when wanted, laid out for the screen and the squad; the battle's
 * screen put right for it (the map shown in the drone's window while the 3D view plays in its own, the game's panel
 * under the drone, the map's camera in the drone's window, following the soldier looked through); the strip of cards
 * brought to the selected soldier (and to one the aliens' turn pops a camera up for); the crosshair in the 3D window's
 * middle; the equipment window's picture; the minimap's frame. Not wanted: the battle's screen as it was.
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
	_state->_warning->setY(_warningY);

	// The strip: from its first soldier shown; brought to the selected soldier when another is selected, and to one the
	// aliens' turn pops a camera up for (see alienTurnMapCameras: its card says why).
	const std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
	const int count = (int)squad.size(), shown = feed.cardsShown;
	auto bringIn = [&](const BattleUnit *unit) {
		for (int place = 0; place < count; ++place)
		{
			if (squad[place].first != unit) continue;
			if (place < feed.first) feed.first = place;
			if (place >= feed.first + shown) feed.first = place - shown + 1;
		}
	};
	BattleUnit *selected = _save->getSelectedUnit();
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

	feedButtons();
	placeFeedScreen();

	// The drone follows the soldier looked through: on them when another is looked through, and again when they walk
	// out of its picture (the map follows only the aliens it sees).
	BattleUnit *looked = isActive() && _trackedUnit ? _trackedUnit : (selected && selected->getFaction() == FACTION_PLAYER ? selected : nullptr);
	Camera *camera = _state->_map->getCamera();
	if (looked && !looked->isOut() && looked->getTile()
		&& (looked != feed.followed || !camera->isOnScreen(looked->getPosition(), true, looked->getArmor()->getSize() - 1, false)))
	{
		camera->centerOnPosition(looked->getPosition(), false);
	}
	feed.followed = looked;

	// The crosshair (and the words beside it, placed by it: see updateCrosshair) in the middle of the 3D window.
	const SDL_Rect &picture = feed.viewPicture;
	_crosshair->setX((int)std::lround(toCanvasX(picture.x + picture.w / 2.0)) - CrosshairSize / 2);
	_crosshair->setY((int)std::lround(toCanvasY(picture.y + picture.h / 2.0)) - CrosshairSize / 2);

	makeFeedEquipment(selected && selected->getFaction() == FACTION_PLAYER && !selected->isOut() ? selected : nullptr);

	// The minimap's frame (its clock, drawn again as the battle goes on).
	if (feed.minimap && feed.maximised == CommandFeed::MAX_NONE)
	{
		feed.minimap->think();
	}
}

/**
 * The Live command feed leaves the screen (the full 3D view, or its setting went off): the game's panel back in its
 * place, the map's camera on the whole screen again around what it looked at, the map hidden again under the 3D view,
 * the crosshair back in the middle, and the feed's windows let go (made again when it comes back).
 */
void FirstPersonMode::leaveFeed()
{
	CommandFeed &feed = *_feed;
	moveGamePanel(-feed.shift);
	feed.shift = 0;
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
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	_crosshair->setX(canvasWidth() / 2 - CrosshairSize / 2);
	_crosshair->setY(canvasHeight() / 2 - CrosshairSize / 2);
	feed.minimap.reset();
	feed.minimapWindow = SDL_Rect{};
	feed.equipmentSurface.reset();
	feed.equipmentKey.clear();
	feed.maximised = CommandFeed::MAX_NONE;
	feed.width = 0; // (laid out again when it comes back)
	feed.followed = nullptr;
	feed.pressedOnDrone = feed.overDrone = false;
	feed.on = false;
}

/**
 * Lays the feed out for the screen (in the game's screen pixels), again only when the screen, the top bar's foot or
 * the squad's count changed: the strip of soldiers' cards under the top bar (as many as fit, with arrows at its ends
 * when the squad has more); under it, down to FeedUpperShare of the screen, the equipment window at the left (a third
 * of the width) and the 3D view's window; below, the drone's window at the left and the minimap's at the right (if
 * there is room), and the drone's picture all over the screen. The minimap is made for its window.
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

	// The strip of cards: as many as fit, with arrows when the squad has more.
	const int cardW = FeedCardW * ui, cardH = FeedCardH * ui;
	feed.strip = makeRect(0, top, width, cardH);
	int arrows = 0;
	int shown = std::min(soldiers, std::max(1, (width + gap) / (cardW + gap)));
	if (soldiers > shown)
	{
		arrows = FeedArrowW * ui + gap;
		shown = std::min(soldiers, std::max(1, (width - 2 * arrows + gap) / (cardW + gap)));
	}
	feed.cardsShown = shown;
	feed.cardsLeft = arrows + (width - 2 * arrows - (shown * cardW + std::max(0, shown - 1) * gap)) / 2;

	// The equipment window and the 3D view's.
	const int upperTop = top + cardH + gap;
	const int upperBottom = std::max(upperTop + 4 * titleH, (int)(height * FeedUpperShare));
	const int equipmentW = width / 3;
	feed.equipment = makeRect(0, upperTop, equipmentW, upperBottom - upperTop);
	feed.equipmentPicture = makeRect(border, upperTop + border + titleH, equipmentW - 2 * border, upperBottom - upperTop - 2 * border - titleH);
	feed.view = makeRect(equipmentW, upperTop, width - equipmentW, upperBottom - upperTop);
	feed.viewPicture = makeRect(equipmentW + border, upperTop + border + titleH, width - equipmentW - 2 * border, upperBottom - upperTop - 2 * border - titleH);

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
	feed.equipmentKey.clear(); // (drawn again for its window)

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
 * The feed's buttons for this frame, in the windows' title lines (filled from the right): the drone's night vision,
 * names, boxes and all over the screen; the minimap's all over the screen; the 3D window's, which goes to the full 3D
 * view; the equipment window's INVENTORY; the strip's arrows; and the equipment window's own (see makeFeedEquipment).
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
	add(CommandFeed::FB_NIGHT, drone, right, _state->tr("STR_FPS_FEED_NIGHT"), feed.nightVision);
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
	int equipmentRight = feed.equipment.x + feed.equipment.w - border;
	add(CommandFeed::FB_INVENTORY, feed.equipment, equipmentRight, _state->tr("STR_FPS_FEED_INVENTORY"), true);
	// (the strip's arrows, when the squad has more soldiers than cards)
	if (feed.soldiers > feed.cardsShown)
	{
		const int arrowW = FeedArrowW * ui;
		feed.buttons.push_back({ CommandFeed::FB_LEFT, makeRect(0, feed.strip.y, arrowW, feed.strip.h), "<", 0, feed.first > 0 });
		feed.buttons.push_back({ CommandFeed::FB_RIGHT, makeRect(feed.width - arrowW, feed.strip.y, arrowW, feed.strip.h), ">", 0,
			feed.first + feed.cardsShown < feed.soldiers });
	}
	feed.buttons.insert(feed.buttons.end(), feed.equipmentButtons.begin(), feed.equipmentButtons.end());
}

/**
 * Puts the battle's screen right for the feed: the game's panel (its buttons, bars and words) moved under the middle
 * of the drone's picture, and the map's camera told to show its middle there (see Camera::setViewWindow): what it
 * looked at stays in the middle of what shows.
 */
void FirstPersonMode::placeFeedScreen()
{
	CommandFeed &feed = *_feed;
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
 * An item's inventory picture (the game's own, BIGOBS.PCK, with the item's colours and frame) drawn on a surface,
 * magnified or made smaller by whole steps.
 * @param target Where to draw it.
 * @param item The item.
 * @param x, y Its top left corner.
 * @param scale Pixels a picture pixel takes (the feed's magnification).
 * @param shrink Picture pixels a drawn pixel stands for (1: whole; 2: half its size).
 */
void FirstPersonMode::drawFeedItem(SDL_Surface *target, const BattleItem *item, int x, int y, int scale, int shrink)
{
	CommandFeed &feed = *_feed;
	SurfaceSet *pictures = _game->getMod()->getSurfaceSet("BIGOBS.PCK");
	if (!item || !pictures)
	{
		return;
	}
	if (!feed.itemPicture)
	{
		feed.itemPicture.reset(new Surface(FeedItemW, FeedItemH, 0, 0));
	}
	Surface &picture = *feed.itemPicture;
	picture.setPalette(_state->getPalette());
	picture.clear();
	item->getRules()->drawHandSprite(pictures, &picture, item, _save);
	for (int pictureY = 0; pictureY < FeedItemH; pictureY += shrink)
	{
		for (int pictureX = 0; pictureX < FeedItemW; pictureX += shrink)
		{
			const Uint8 pixel = picture.getPixel(pictureX, pictureY);
			if (pixel) fill(target, x + pictureX / shrink * scale, y + pictureY / shrink * scale, scale, scale, pixel);
		}
	}
}

/**
 * The equipment window's picture for a soldier, drawn again only when what it shows changed (the soldier, their items
 * and rounds, the hand and fire mode in use, whether they can act): the two hands (and the built-in weapon, if any),
 * each with the item's inventory picture, its name and rounds, the hand in use lit (a click uses it); the fire modes of
 * the weapon in use (a click picks one); the other items carried, small. Its buttons are kept for the clicks (see
 * feedButtons; INVENTORY, the game's inventory, is in the window's title line).
 * @param unit The soldier (null: none to show).
 */
void FirstPersonMode::makeFeedEquipment(BattleUnit *unit)
{
	CommandFeed &feed = *_feed;
	const SDL_Rect &area = feed.equipmentPicture;
	std::ostringstream key;
	key << area.x << ',' << area.y << ',' << area.w << ',' << area.h << ' ' << (unit ? unit->getId() : -1);
	if (unit)
	{
		key << ' ' << activeHand(unit) << ' ' << (int)_fireMode << ' ' << canAct();
		for (const BattleItem *item : *unit->getInventory())
		{
			const BattleItem *ammo = item->getAmmoForSlot(0);
			key << ' ' << item->getId() << ':' << (item->getSlot() ? item->getSlot()->getId() : std::string()) << ':' << item->getSlotX() << ':' << item->getSlotY()
				<< ':' << (ammo ? ammo->getAmmoQuantity() : item->getAmmoQuantity());
		}
	}
	if (feed.equipmentSurface && key.str() == feed.equipmentKey)
	{
		return;
	}
	feed.equipmentKey = key.str();
	feed.equipmentSurface.reset(new Surface(std::max(1, (int)area.w), std::max(1, (int)area.h), 0, 0));
	feed.equipmentSurface->setPalette(_state->getPalette());
	SDL_Surface *picture = feed.equipmentSurface->getSurface();
	fill(picture, 0, 0, area.w, area.h, feed.titleBack);
	feed.equipmentButtons.clear();
	const int ui = feed.ui, pad = 2 * ui, lineH = FeedWordsH * ui;
	if (!unit)
	{
		feedWrite(picture, _state->tr("STR_FPS_FEED_NO_SOLDIER"), pad, pad, feed.dimColour);
		return;
	}
	// (a button of the window: drawn here, its place kept on the screen)
	auto button = [&](CommandFeed::ButtonKind kind, int value, const SDL_Rect &box, const std::string &words, bool lit) {
		fill(picture, box.x, box.y, box.w, box.h, feed.black);
		outline(picture, box, ui, lit ? feed.frameLit : feed.frameColour);
		if (!words.empty())
		{
			feed.writer->setText(words);
			const int wide = std::min((int)box.w - 2 * ui, feed.writer->getTextWidth() * ui);
			feedWrite(picture, words, box.x + (box.w - wide) / 2, box.y + (box.h - lineH) / 2 + ui, lit ? feed.wordsColour : feed.dimColour, box.w - 2 * ui);
		}
		feed.equipmentButtons.push_back({ kind, makeRect(area.x + box.x, area.y + box.y, box.w, box.h), std::string(), value, lit });
	};

	// The hands (and the built-in weapon): the item's picture, its name and rounds.
	BattleType specialType;
	std::vector<int> hands = { 0, 1 };
	if (unit->getSpecialIconWeapon(specialType)) hands.push_back(2);
	const int used = activeHand(unit);
	const int slotW = (area.w - pad * ((int)hands.size() + 1)) / (int)hands.size(), slotH = FeedItemH * ui + 2 * pad;
	int y = pad;
	for (size_t place = 0; place < hands.size(); ++place)
	{
		const int hand = hands[place];
		const SDL_Rect box = makeRect(pad + (int)place * (slotW + pad), y, slotW, slotH);
		BattleItem *item = handWeapon(unit, hand);
		button(CommandFeed::FB_HAND, hand, box, std::string(), hand == used);
		const int pictureW = std::min(FeedItemW * ui, (int)box.w / 2);
		if (item) drawFeedItem(picture, item, box.x + pad, box.y + pad, ui, 1);
		const int wordsX = box.x + pad + pictureW + pad, room = box.x + box.w - pad - wordsX;
		static const char *handNames[] = { "STR_FPS_FEED_RIGHT", "STR_FPS_FEED_LEFT", "STR_FPS_FEED_BUILT_IN" };
		feedWrite(picture, _state->tr(handNames[hand]), wordsX, box.y + pad, hand == used ? feed.pickedColour : feed.dimColour, room);
		if (item)
		{
			feedWrite(picture, itemLabel(item), wordsX, box.y + pad + lineH + ui, feed.wordsColour, room);
			bool loaded = true;
			std::string rounds = ammoLabel(unit, item, BA_NONE, loaded, nullptr);
			rounds.erase(std::remove(rounds.begin(), rounds.end(), Unicode::TOK_COLOR_FLIP), rounds.end());
			if (!rounds.empty()) feedWrite(picture, rounds, wordsX, box.y + pad + 2 * (lineH + ui), loaded ? feed.hudColour : feed.liveColour, room);
		}
		else
		{
			feedWrite(picture, _state->tr("STR_FPS_FEED_EMPTY"), wordsX, box.y + pad + lineH + ui, feed.dimColour, room);
		}
	}
	y += slotH + pad;

	// The fire modes of the weapon in use (its name as the crosshair's line has it).
	static const struct { BattleActionType mode; const char *name; } modeNames[] = {
		{ BA_SNAPSHOT, "STR_FPS_MODE_SNAP" }, { BA_AIMEDSHOT, "STR_FPS_MODE_AIMED" }, { BA_AUTOSHOT, "STR_FPS_MODE_AUTO" },
		{ BA_THROW, "STR_FPS_MODE_THROW" }, { BA_LAUNCH, "STR_FPS_MODE_LAUNCH" }, { BA_HIT, "STR_FPS_MODE_MELEE" },
		{ BA_PANIC, "STR_FPS_MODE_PANIC" }, { BA_MINDCONTROL, "STR_FPS_MODE_MIND_CONTROL" }, { BA_USE, "STR_FPS_MODE_USE" },
		{ BA_PRIME, "STR_FPS_MODE_PRIME" }, { BA_UNPRIME, "STR_FPS_MODE_UNPRIME" } };
	BattleItem *weapon = activeWeapon(unit);
	int x = pad;
	const int modeH = lineH + 2 * ui;
	for (const BattleActionType mode : weapon ? fireModesFor(unit, weapon) : std::vector<BattleActionType>())
	{
		std::string words;
		for (const auto &entry : modeNames) if (entry.mode == mode) words = _state->tr(entry.name);
		const std::string renamed = renamedAction(weapon->getRules(), mode);
		if (!renamed.empty()) words = renamed;
		if (words.empty()) continue;
		feed.writer->setText(words);
		const int wide = feed.writer->getTextWidth() * ui + 2 * FeedButtonPad * ui;
		if (x + wide > area.w - pad && x > pad)
		{
			x = pad;
			y += modeH + ui;
		}
		button(CommandFeed::FB_MODE, (int)mode, makeRect(x, y, std::min(wide, (int)area.w - 2 * pad), modeH), words, mode == _fireMode);
		x += wide + ui;
	}
	if (x > pad) y += modeH + pad;

	// The other items carried, small, as many as there is room for (INVENTORY is in the window's title line).
	const int iconW = FeedItemW / 2 * ui, iconH = FeedItemH / 2 * ui;
	const int columns = std::max(1, (area.w - pad) / (iconW + ui)), rows = std::max(0, (area.h - pad - y) / (iconH + ui));
	std::vector<const BattleItem*> carried;
	for (const BattleItem *item : *unit->getInventory())
	{
		if (item == unit->getRightHandWeapon() || item == unit->getLeftHandWeapon() || (item->getSlot() && item->getSlot()->getType() == INV_HAND)) continue;
		carried.push_back(item);
	}
	const int room = columns * rows;
	for (int place = 0; place < (int)carried.size() && place < room; ++place)
	{
		const bool last = place == room - 1 && (int)carried.size() > room;
		const int iconX = pad + (place % columns) * (iconW + ui), iconY = y + (place / columns) * (iconH + ui);
		if (last)
		{
			feedWrite(picture, "+" + std::to_string((int)carried.size() - room + 1), iconX, iconY + (iconH - lineH) / 2, feed.dimColour);
			break;
		}
		fill(picture, iconX, iconY, iconW, iconH, feed.black);
		drawFeedItem(picture, carried[place], iconX, iconY, ui, 2);
	}
}

/**
 * Draws the strip of soldiers' cards: each the soldier's number and name (the selected one's lit), bars of their time
 * units, health and energy, and the weapon in use; framed red, and saying why, for a soldier the aliens' turn pops a
 * camera up for. The cards' places are kept for the clicks.
 * @param screen The game's screen.
 */
void FirstPersonMode::drawFeedCards(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, pad = 2 * ui, lineH = FeedWordsH * ui, cardW = FeedCardW * ui, gap = FeedGap * ui;
	const std::vector<std::pair<BattleUnit*, int>> squad = feedSquad();
	const BattleUnit *alerted = _alertUnit && SDL_GetTicks() < _alertUntil && !_alertText.empty() ? _alertUnit : nullptr;
	feed.cards.clear();
	for (int card = 0; card < feed.cardsShown && feed.first + card < (int)squad.size(); ++card)
	{
		BattleUnit *unit = squad[feed.first + card].first;
		const SDL_Rect box = makeRect(feed.cardsLeft + card * (cardW + gap), feed.strip.y, cardW, feed.strip.h);
		feed.cards.push_back({ unit, squad[feed.first + card].second, box });
		const bool selected = unit == _save->getSelectedUnit();
		fill(screen, box.x, box.y, box.w, box.h, feed.titleBack);
		outline(screen, box, ui, unit == alerted ? feed.liveColour : selected ? feed.frameLit : feed.frameColour);
		const int room = box.w - 2 * pad;
		feedWrite(screen, std::to_string(squad[feed.first + card].second) + " " + unit->getName(_game->getLanguage(), false), box.x + pad, box.y + pad,
			selected ? feed.pickedColour : feed.wordsColour, room);
		// (the bars: time units, health, energy, each against its full value)
		const UnitStats *full = unit->getBaseStats();
		const int values[3] = { unit->getTimeUnits(), unit->getHealth(), unit->getEnergy() }, most[3] = { full->tu, full->health, full->stamina };
		const Uint8 colours[3] = { feed.timeColour, feed.healthColour, feed.energyColour };
		for (int bar = 0; bar < 3; ++bar)
		{
			const int barY = box.y + pad + lineH + ui + bar * 3 * ui, length = most[bar] > 0 ? room * std::clamp(values[bar], 0, most[bar]) / most[bar] : 0;
			fill(screen, box.x + pad, barY, room, 2 * ui, feed.barBack);
			fill(screen, box.x + pad, barY, length, 2 * ui, colours[bar]);
		}
		const int lowerY = box.y + pad + lineH + 10 * ui;
		if (unit == alerted)
		{
			feedWrite(screen, _state->tr(_alertText), box.x + pad, lowerY, feed.liveColour, room);
		}
		else if (const BattleItem *weapon = activeWeapon(unit))
		{
			feedWrite(screen, itemLabel(weapon), box.x + pad, lowerY, feed.dimColour, room);
		}
	}
}

/**
 * Does the battle's blit (see blit) draw this surface of the battle's own with the feed showing? Not the map (the feed
 * draws it, in the drone's window), nor those at the screen's edges (drawn after the windows, over them).
 * @param surface The surface.
 * @return True if the blit draws it.
 */
bool FirstPersonMode::feedDrawsGameSurface(const Surface *surface) const
{
	return surface != _state->_map && !feedEdgeSurface(surface);
}

/**
 * Which of our own overlays show with the feed (see drawOverlays): the top bar without the squad's buttons (the strip
 * of cards stands for them), and in the 3D window, while the 3D view plays there, the crosshair and the words beside
 * it (the shot, its odds and cost, the range, the unit pointed at): the rest of the 3D HUD the windows show.
 * @param surface Our surface.
 * @return True if it shows.
 */
bool FirstPersonMode::feedOverlay(const Surface *surface) const
{
	Surface *own = const_cast<Surface*>(surface);
	if (_topBarSurfaces.count(own))
	{
		return own != _btnPrev && own != _btnNext && std::find(_btnSquad.begin(), _btnSquad.end(), own) == _btnSquad.end()
			&& std::find(_squadIcons.begin(), _squadIcons.end(), own) == _squadIcons.end();
	}
	const bool viewPlays = isActive() && _feed->maximised == CommandFeed::MAX_NONE;
	return viewPlays && (own == _crosshair || own == _txtMode || own == _txtShot || own == _txtDamage || own == _txtRange || own == _txtAimOff
		|| own == _txtTarget || own == _txtCosts);
}

/**
 * Draws what of the feed goes under the game's panel: the map in the drone's picture (as the game drew it: only that
 * part of it shows), turned into the drone's feed (see filterDrone), and the marks over the soldiers.
 * @param screen The game's screen.
 */
void FirstPersonMode::blitFeedUnder(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
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
 * Draws the feed's windows over the battle's screen: the strip of cards and its arrows; the equipment window; the 3D
 * window (titled with the soldier looked through, a red dot that blinks, and whether the mouse plays there: the 3D view
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
		drawFeedCards(screen);

		// The equipment window.
		const BattleUnit *selected = _save->getSelectedUnit();
		drawFeedWindow(screen, feed.equipment, feed.frameColour);
		titleWords(feed.equipment, _state->tr("STR_FPS_FEED_EQUIPMENT"), feed.wordsColour, false);
		if (feed.equipmentSurface)
		{
			SDL_Rect where = feed.equipmentPicture;
			SDL_BlitSurface(feed.equipmentSurface->getSurface(), nullptr, screen, &where);
		}

		// The 3D window: the 3D view at the window's size, or no signal.
		const bool plays = isActive() && _view->getVisible();
		drawFeedWindow(screen, feed.view, plays && wantsMouseCapture() ? feed.frameLit : feed.frameColour);
		const BattleUnit *looked = plays && _trackedUnit ? _trackedUnit : selected;
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
	// when on (the equipment window drew its own).
	for (const CommandFeed::Button &button : feed.buttons)
	{
		if (button.kind == CommandFeed::FB_HAND || button.kind == CommandFeed::FB_MODE) continue;
		if (feed.maximised != CommandFeed::MAX_NONE && button.kind != CommandFeed::FB_NIGHT && button.kind != CommandFeed::FB_NAMES && button.kind != CommandFeed::FB_BOXES
			&& button.kind != CommandFeed::FB_DRONE_MAX) continue;
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
 *  - the feed's buttons (the windows', the equipment's): a left press works one;
 *  - the minimap: its own (all the tactical map's buttons, its drag and wheel);
 *  - the strip of cards: a left click selects the soldier, the wheel scrolls it;
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

	if (feed.pressedOnDrone)
	{
		if (event.type == SDL_MOUSEBUTTONUP) feed.pressedOnDrone = false;
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
		const bool shown = feed.maximised == CommandFeed::MAX_NONE || each.kind == CommandFeed::FB_NIGHT || each.kind == CommandFeed::FB_NAMES
			|| each.kind == CommandFeed::FB_BOXES || each.kind == CommandFeed::FB_DRONE_MAX;
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
		if (inside(feed.strip, x, y))
		{
			if (wheel != 0)
			{
				scrollFeed(-wheel);
			}
			else if (leftPress)
			{
				for (const CommandFeed::Card &card : feed.cards)
				{
					if (inside(card.box, x, y)) selectUnit(card.unit);
				}
			}
			return true;
		}
		if (inside(feed.equipment, x, y))
		{
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
		return false;
	}
	return feedWithoutMap(action);
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
	if (TextButton::soundPress) TextButton::soundPress->play(Mix_GroupAvailable(0));
	switch (button.kind)
	{
	case CommandFeed::FB_NIGHT: feed.nightVision = !feed.nightVision; break;
	case CommandFeed::FB_NAMES: feed.names = !feed.names; break;
	case CommandFeed::FB_BOXES: feed.boxes = !feed.boxes; break;
	case CommandFeed::FB_DRONE_MAX: feed.maximised = feed.maximised == CommandFeed::MAX_DRONE ? CommandFeed::MAX_NONE : CommandFeed::MAX_DRONE; break;
	case CommandFeed::FB_MAP_MAX: showTacticalMap(); break;
	case CommandFeed::FB_VIEW_MAX: toggleView(); return; // (to the full 3D view: the feed is let go of)
	case CommandFeed::FB_LEFT: scrollFeed(-1); break;
	case CommandFeed::FB_RIGHT: scrollFeed(1); break;
	case CommandFeed::FB_HAND: chooseHand(button.value); break;
	case CommandFeed::FB_MODE: selectFireMode((BattleActionType)button.value); break;
	case CommandFeed::FB_INVENTORY: _state->btnInventoryClick(action); return;
	}
	feedButtons();
}

/**
 * Uses the item in one hand (or the built-in weapon), as the hand key's turn does (see switchHand): picked in the
 * equipment window.
 * @param hand 0 right, 1 left, 2 the built-in weapon.
 */
void FirstPersonMode::chooseHand(int hand)
{
	BattleUnit *unit = _save->getSelectedUnit();
	if (!unit || !canAct() || !handWeapon(unit, hand) || activeHand(unit) == hand)
	{
		return;
	}
	_state->_battleGame->cancelCurrentAction();
	endHitLogPick();
	_chosenHand[unit] = hand;
	_fireModeWeapon = nullptr; // (a fresh fire mode for what is in that hand)
}

/**
 * Scrolls the strip of cards through the squad.
 * @param by Soldiers on (less than 0: back).
 */
void FirstPersonMode::scrollFeed(int by)
{
	CommandFeed &feed = *_feed;
	feed.first = std::clamp(feed.first + by, 0, std::max(0, feed.soldiers - feed.cardsShown));
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
	feed.timeColour = bright(60, 200, 70);
	feed.healthColour = bright(230, 50, 50);
	feed.energyColour = bright(240, 160, 40);
	feed.barBack = nearestColor(palette, 40, 44, 56);
	if (!feed.writer)
	{
		feed.writer.reset(new Text(Options::baseXResolution, FeedWordsH, 0, 0));
		feed.writer->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
		feed.writer->setSmall();
	}
	feed.writer->setPalette(palette);
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
