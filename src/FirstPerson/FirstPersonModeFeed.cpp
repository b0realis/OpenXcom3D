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
#include "../Engine/Screen.h"
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
#include "../Savegame/HitLog.h"
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
const double FeedUpperShare = 0.5, FeedInventoryShare = 0.45; // (the bottom half a bit bigger: the owner, "lets increase the bottom half of the screen bit")
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
/// The inventory window's picture is made again at the inventory's own animation's pace (its items pulse, its warnings
/// fade), at once when it changed (another soldier, an item held or moved, a click there).
const Uint32 FeedInventoryEveryMs = 100;
/// The squad tab's table: a row this high (times the magnification) under a line of column heads, its scroll bar this
/// wide, its columns this far apart, a name at least this wide (else health, energy and morale go, morale first).
const int FeedRowH = 13, FeedScrollW = 4, FeedColumnGap = 4, FeedNameLeast = 36;
/// A step of the 3D view's walked on the drone's map: the game's ticks round its phases (a turn, the start, the end; see
/// feedStepPhaseMs).
const int FeedStepTicks = 4;
/// HUD: the game's panel rises into the drone's window (or sinks out of it) in this many milliseconds.
const double FeedHudSlideMs = 250.0;
/// The squad tab's values: low under this share of their most (on an amber ground), critical under this one (on red).
const double FeedLowShare = 0.5, FeedCriticalShare = 0.25;
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
	enum ButtonKind { FB_NIGHT, FB_THERMAL, FB_STABILISER, FB_NAMES, FB_BOXES, FB_DRONE_MAX, FB_FLY_TO, FB_FOLLOW, FB_DYNAMIC, FB_MOVE_UP, FB_MOVE_DOWN, FB_MOVE_LEFT, FB_MOVE_RIGHT,
		FB_LEVEL_UP, FB_LEVEL_DOWN, FB_MAP_MAX, FB_VIEW_MAX, FB_INVENTORY, FB_TAB_SQUAD, FB_TAB_INVENTORY, FB_ISO, FB_TAB_HITLOG, FB_HUD };
	/// The window at the top left: the squad's table, the selected soldier's inventory or the hit log (the player's choice).
	enum Tab { TAB_SQUAD, TAB_INVENTORY, TAB_HITLOG };
	/// A line of the hit log tab: a turn's start, a firing (the weapon, then its shots), reaction fire (then its shots), or
	/// shots with no firing before them (the enemy's, in their turn); its parts, each in a style of its own (see
	/// drawFeedHitLog: the words grey, the hits' words emphasised).
	enum HitLineKind { HIT_TURN, HIT_FIRING, HIT_REACTION, HIT_SHOTS };
	enum HitStyle { HS_TEXT, HS_HEADER, HS_ZERO, HS_HIT, HS_BIG_HIT, HS_KILLED, HS_DOWN, HS_UNSEEN };
	struct HitPart
	{
		std::string text;
		HitStyle style;
		bool glued = false; // (as drawn: right after the word before, with no space: "(unseen)", "killed)")
	};
	struct HitLine
	{
		std::vector<HitPart> parts;
		HitLineKind kind;
		int faction;
		bool shot; // (a shot is in it already)
	};
	/// A row of the hit log tab as drawn: a line's words that fit the window's width (in their styles; its first row with
	/// the mark of the side that fired), a turn's heading (its side's mark before it), or the rule between two turns.
	struct HitRow
	{
		std::vector<HitPart> words;
		bool heading, rule;
		int faction;
		bool lineStart = false;
	};
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
	// ISO (the owner: an emerald button that shows "normal iso where you can do everything like in the normal game", then
	// "just draws the normal iso map in its bottom left window - no resizing"): the drone's window shows the isometric map
	// as the game draws it, in its own colours, and the keys are the battle's (see feedIsoHasKeys).
	bool iso = false;
	// HUD (the owner, play-testing: "a HUD button (colored purple) besides iso - that brings up smoothly the hud so tester
	// can have full normal game control"): with ISO, the game's own panel in the drone's window, at its foot, rising into
	// view: wanted or not, how far up it is (0 down and hidden .. 1 up), and the battle's surfaces as the game had placed
	// them (put back when it goes); the map's camera told the panel is there.
	bool hud = false;
	double hudShown = 0.0;
	std::vector<std::pair<Surface*, std::pair<int, int>>> hudPlaces;
	bool cameraIcons = false;
	std::vector<Button> buttons;
	// Flying the drone: the arrow held (one of FB_MOVE_*, else none), and a glide to a soldier (FLY TO): from where to
	// where (the map's offsets), since when; the mouse's last point (to light what it is over); the last frame's time.
	int moving = -1;
	double flyRest = 0.0;
	bool gliding = false, follow = false;
	// FOLLOW: whom it followed last frame and on which level (their level is taken again only when either changes: the
	// level buttons work while following).
	const BattleUnit *followedUnit = nullptr;
	int followedZ = -1;
	// DYNAMIC: the battle's own moves of the map's camera move the drone too (on: as on the isometric map), or it stays
	// where the player left it (off, at first: the owner, "If you dont have dynamic option in the feed the view should not
	// follow shots! Only in dynamic that replicates iso mode"); off, where it was before the battle's think this frame (noted, see
	// beforeBattleThink), and the map told not to go after shots (to tell back when it comes on again); where it was at
	// the end of the last frame, and whether the battle put a screen over it then (the next turn's, which centres the
	// map on the soldier as it goes).
	bool dynamic = false, held = false, projectileHeld = false, screenFromBattle = false;
	Position heldAt, endAt;
	Position glideFrom, glideTo;
	Uint32 glideStart = 0, lastFrame = 0;
	double frameMs = 1000.0 / 60; // (a frame's time, smoothed: how quick a step can play on the map, see feedStepPhaseMs)
	int mouseX = -1, mouseY = -1;
	Maximised maximised = MAX_NONE;
	// The minimap: the tactical map in its window (made again only for another window).
	std::unique_ptr<FirstPersonMapState> minimap;
	// The feed's own screen in the palette, when the game's is in full colour (see blitFeed).
	std::unique_ptr<Surface> paletteScreen;
	SDL_Rect minimapWindow{};
	// The inventory window: the game's own inventory (its grid and items, moved by the mouse as on its screen), drawn
	// with the screen's background and the soldier's figure into a picture of the screen's size, shown scaled into the
	// window (source columns and rows for each shown one); whose, and what of theirs it was last arranged for.
	std::unique_ptr<Inventory> inventoryGrid;
	std::unique_ptr<Surface> inventoryScreen, doll;
	std::vector<int> inventoryColumns, inventoryRows;
	BattleUnit *inventoryUnit = nullptr;
	std::string inventoryKey;
	Uint32 inventoryDrawn = 0;
	bool inventoryTouched = false, noInventory = false;
	Position inventoryAt;
	// The squad tab: its first row shown, its scroll bar's thumb dragged (from where, at which row), the soldier last kept
	// in sight; this frame's squad (as the squad buttons number it), the rows' place, how many show, the bar's track and
	// thumb.
	Tab tab = TAB_SQUAD;
	// The hit log tab (the owner: "Top left window should have another tab: hit-log (expanded hit-log updating realtime)",
	// then "the log should keep all turns"): the battle's log as the game writes it, the aliens' turns too, each hit with
	// whom it hurt (see hitLogged); its lines wrapped into the window's rows (made again when a line changed, or the
	// window), and how many rows the wheel scrolled it back (0: the newest at the foot).
	std::vector<HitLine> hitLines;
	std::string hitWeapon; // (the player's last firing: their shots after the enemy's go on a line of it again, as in the game's log)
	int hitVersion = 0, hitRowsVersion = -1, hitRowsWidth = 0, hitScroll = 0;
	std::vector<HitRow> hitRows;
	// (each wrapped line's first row: a new entry wraps only the last line, which may have grown, and those after it; all
	// again when the window's width changed or the oldest lines went, hitErased counting that)
	std::vector<size_t> hitLineRow;
	int hitErased = 0, hitRowsErased = 0;
	int squadScroll = 0;
	bool squadDragging = false;
	int dragFromY = 0, dragFromScroll = 0;
	const BattleUnit *squadSeen = nullptr;
	std::vector<BattleUnit*> squadUnits;
	SDL_Rect squadRows{}, squadTrack{}, squadThumb{};
	int squadShown = 1;
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
	bool warningKnown = false; // (its own place noted, before the feed moved it: the panel is shown and hidden again for ISO's keys)
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
	Uint8 purple = 0, purpleLit = 0, turnColour = 0, clockColour = 0, dangerColour = 0, rowBack = 0;
	Uint8 tuColour = 0, healthColour = 0, energyColour = 0, moraleColour = 0, lowBack = 0, lowWords = 0, criticalBack = 0;
	Uint8 emerald = 0, emeraldLit = 0;
	Uint8 logText = 0, logHit = 0, logBigHit = 0, logDown = 0, xcomMark = 0, enemyMark = 0;
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
	return kind == CommandFeed::FB_ISO || kind == CommandFeed::FB_HUD || kind == CommandFeed::FB_NIGHT || kind == CommandFeed::FB_THERMAL || kind == CommandFeed::FB_STABILISER || kind == CommandFeed::FB_NAMES
		|| kind == CommandFeed::FB_BOXES || kind == CommandFeed::FB_DRONE_MAX || kind == CommandFeed::FB_FLY_TO || kind == CommandFeed::FB_FOLLOW || kind == CommandFeed::FB_DYNAMIC || kind == CommandFeed::FB_MOVE_UP
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
 * Are the keys the battle's for the feed's ISO: its drone window showing the isometric map (see pressFeedButton), and the
 * 3D window not having the mouse (a click there gives it the mouse and the keys again)? The keys are then the battle's
 * as on the isometric map, not the 3D view's (see handle), and the 3D window walks no one.
 * @return True if so.
 */
bool FirstPersonMode::feedIsoHasKeys() const
{
	return feedShown() && _feed->iso && !_mouseLook;
}

/**
 * A key the battle takes with the feed's ISO, as on the isometric map. The game's shortcuts are its panel's buttons'
 * (centre on the soldier, kneel, the next soldier, the levels...), which take keys only while shown, and the panel is
 * hidden in the feed: it is shown for the key alone (nothing is drawn meanwhile), and the map stays where it is.
 * @param action The key.
 * @return True (it was dealt with).
 */
bool FirstPersonMode::feedIsoKey(Action *action)
{
	releaseFeedItem(); // (a key may open the game's inventory, or throw: an item held in the window goes back first)
	const bool overIcons = _state->_mouseOverIcons; // (where the mouse is stays as it was: the map's cursor shows or not)
	const bool panelUp = _feed->hudShown > 0.0; // (HUD: shown already)
	if (!panelUp) hideGamePanel(false);
	_feedDispatching = true;
	_state->handle(action);
	_feedDispatching = false;
	if (!panelUp) hideGamePanel(true);
	_state->_mouseOverIcons = overIcons;
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
	// DYNAMIC off (the owner: "On keeps, off stays as player positioned"): what the battle itself did with the map's
	// camera this frame (after a soldier walking, a shot, an explosion, the aliens in their turn, a turn's start) is
	// taken back, before the layout (which keeps the middle of the drone's picture for another window). The player's own
	// moves came before the battle's think and stay (the arrows, the level buttons and keys, the map's scrolling and
	// wheel, the tactical map; FLY TO and FOLLOW below). The map no longer goes after a shot either (Map::draw would move
	// its camera with the bullet), until DYNAMIC is on again (not in the middle of a shot: its own end tells it again).
	Camera *droneCamera = _state->_map->getCamera();
	if (feed.held)
	{
		feed.held = false;
		const Position moved = droneCamera->getMapOffset();
		if (moved.x != feed.heldAt.x || moved.y != feed.heldAt.y || moved.z != feed.heldAt.z)
		{
			droneCamera->setMapOffset(feed.heldAt);
			droneCamera->scrollXY(0, 0, true); // (its middle tile again, the cursor on its tile, the map drawn again)
		}
	}
	if (!feed.dynamic && !feed.iso)
	{
		_state->_map->setFollowProjectile(false);
		feed.projectileHeld = true;
	}
	else if (feed.projectileHeld && !_state->_map->getProjectile())
	{
		_state->_map->setFollowProjectile(true);
		feed.projectileHeld = false;
	}
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
	const bool masked = !feed.iso && (feed.vision == CommandFeed::VISION_THERMAL || feed.vision == CommandFeed::VISION_NIGHT_PLUS); // (ISO: the game's own colours)
	if (masked && (!feed.mask || feed.mask->getWidth() != map->getWidth() || feed.mask->getHeight() != map->getHeight()))
	{
		feed.mask.reset(new Surface(map->getWidth(), map->getHeight(), 0, 0));
		feed.mask->setPalette(_state->getPalette());
		map->invalidate();
	}
	map->setMaskTarget(masked ? feed.mask.get() : nullptr, feed.vision == CommandFeed::VISION_NIGHT_PLUS);

	// The player flies the drone (the owner: "Otherwise it stays as player set it"; the battle's own camera moves only with
	// DYNAMIC on, see above): an arrow held flies it that way; FLY TO glides it to the soldier (see flyFeedTo).
	Camera *camera = map->getCamera();
	const Uint32 now = SDL_GetTicks();
	const double seconds = feed.lastFrame ? std::min<Uint32>(100, now - feed.lastFrame) / 1000.0 : 0.0;
	feed.lastFrame = now;
	if (seconds > 0.0) feed.frameMs += (seconds * 1000.0 - feed.frameMs) * 0.1;
	// HUD: the game's panel rising into the drone's window, or sinking out of it.
	{
		const double wanted = feed.iso && feed.hud ? 1.0 : 0.0, by = seconds * 1000.0 / FeedHudSlideMs;
		feed.hudShown = wanted > feed.hudShown ? std::min(wanted, feed.hudShown + by) : std::max(wanted, feed.hudShown - by);
		if (!feed.iso) feed.hudShown = 0.0; // (ISO gone: at once)
	}
	// (a button let go somewhere the feed didn't hear: an arrow, the squad table's thumb, a press on the drone)
	const Uint8 buttons = SDL_GetMouseState(nullptr, nullptr);
	if (!(buttons & SDL_BUTTON(SDL_BUTTON_LEFT)))
	{
		feed.moving = -1;
		feed.squadDragging = false;
	}
	if (!(buttons & (SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(SDL_BUTTON_MIDDLE) | SDL_BUTTON(SDL_BUTTON_RIGHT))))
	{
		feed.pressedOnDrone = false;
	}
	if (feed.moving < 0)
	{
		feed.flyRest = 0.0;
	}
	if (feed.moving >= 0)
	{
		// (whole pixels, the rest carried to the next frame: the same speed at any frame rate)
		feed.flyRest += FeedFlySpeed * feed.ui * seconds;
		const int step = (int)feed.flyRest;
		feed.flyRest -= step;
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
	const BattleUnit *followed = feedLookedUnit();
	if (feed.follow && !feed.iso && !feed.gliding && feed.moving < 0 && _save->getSide() == FACTION_PLAYER && followed && followed->getFaction() == FACTION_PLAYER
		&& !followed->isOut() && followed->getTile())
	{
		const Position at = camera->getMapOffset();
		camera->centerOnPosition(followed->getPosition(), false);
		const Position target = camera->getMapOffset();
		const bool newLevel = followed != feed.followedUnit || followed->getPosition().z != feed.followedZ;
		camera->setMapOffset(Position(at.x, at.y, newLevel ? target.z : at.z));
		feed.followedUnit = followed;
		feed.followedZ = followed->getPosition().z;
		const double share = std::min(1.0, seconds * FeedFollowRate);
		// (at least a pixel a frame, the last at once: it arrives at any frame rate)
		auto toward = [share](int distance) {
			if (std::abs(distance) <= 1) return distance;
			const int by = (int)std::lround(distance * share);
			return by != 0 ? by : (distance > 0 ? 1 : -1);
		};
		camera->scrollXY(toward(target.x - at.x), toward(target.y - at.y), true);
	}
	else
	{
		feed.followedUnit = nullptr;
	}

	// The crosshair (and the words beside it, placed by it: see updateCrosshair) in the middle of the 3D window.
	const SDL_Rect &picture = feed.viewPicture;
	_crosshair->setX((int)std::lround(toCanvasX(picture.x + picture.w / 2.0)) - CrosshairSize / 2);
	_crosshair->setY((int)std::lround(toCanvasY(picture.y + picture.h / 2.0)) - CrosshairSize / 2);

	// (the inventory window's soldier: the one selected; in the aliens' turn the one the 3D window looks through, else the
	// last the player had: shown, its items not moved, see feedInventoryMouse)
	BattleUnit *owner = selected;
	if (_save->getSide() != FACTION_PLAYER)
	{
		owner = isActive() && _trackedUnit && _trackedUnit->getFaction() == FACTION_PLAYER ? _trackedUnit : _lastPlayerUnit;
	}
	// (only a soldier with an inventory, as the game's inventory button has it: not a tank)
	const bool up = owner && owner->getFaction() == FACTION_PLAYER && !owner->isOut() && owner->getTile();
	feed.noInventory = up && !owner->hasInventory();
	if (feed.tab == CommandFeed::TAB_INVENTORY)
	{
		updateFeedInventory(up && !feed.noInventory ? owner : nullptr);
	}
	else if (feed.tab == CommandFeed::TAB_SQUAD)
	{
		updateFeedSquad();
	}

	// (where the drone is at the end of the frame; a screen the battle's think put over it, see beforeBattleThink)
	feed.endAt = camera->getMapOffset();
	feed.screenFromBattle = !_game->isState(_state);
	// The minimap's frame (its clock, drawn again as the battle goes on).
	if (feed.minimap && feed.maximised == CommandFeed::MAX_NONE)
	{
		feed.minimap->think();
	}
}

/**
 * Each frame before the battle's own think (see FirstPerson::beforeBattleThink): with the feed up and its DYNAMIC off,
 * where the drone is now, the player's moves made (their input and the map's own scrolling come before): what the
 * battle does with the map's camera after this is taken back (see updateFeed). After a screen the battle's think put up
 * (the next turn's centres the map on the soldier as it closes), where it was before that screen.
 */
void FirstPersonMode::beforeBattleThink()
{
	if (!_feed)
	{
		return;
	}
	CommandFeed &feed = *_feed;
	feed.held = feed.on && !feed.dynamic && !feed.iso; // (ISO: the battle moves the camera, as on the isometric map)
	if (feed.held)
	{
		// (a screen the battle put up, the next turn's, centred the map as it went: that was the battle's too)
		feed.heldAt = feed.screenFromBattle ? feed.endAt : _state->_map->getCamera()->getMapOffset();
	}
	feed.screenFromBattle = false;
}

/**
 * A step the 3D view walks, with the feed up: how quick its walk plays on the drone's map (the owner: "the camera feed
 * does not show animations. They can be faster animations to conform to 3d no problem - just they must be shown!", then
 * "The walk (all) animations can be sped up ... to conform to 3d"; the 3D view's steps are instant moves, see
 * commitStep). The game walks a step's phases (8, a diagonal one 16) a tick each, at most a tick a frame, round some
 * ticks of its own (a turn to the step's way, the start, the end: FeedStepTicks, an instant step has them too, and 2
 * phases); the next step waits for that walk to end. The 3D eye asks for the next step as it crosses a tile's edge on
 * either axis: walking straight, a tile on; walking aslant (the steps taken in turn on each axis, or a diagonal one),
 * sooner. So the phases come as many to a tick as still end the walk before then (a sprint aslant at 60 frames a second:
 * the step's middle and its end, as the instant step's; walking on, all 8), and the ticks every frame or every few,
 * never slower than the game's own walking speed: the walk shows, and the 3D walk never waits longer for it.
 * @param run Whether the step is run (the sprint's pace, while the eye's own pace isn't known yet).
 * @param diagonal Whether it is a diagonal step.
 * @param phaseStep Set to the phases walked a tick.
 * @return A step phase's milliseconds (a tick's), or 0 for the game's own instant step (no feed).
 */
int FirstPersonMode::feedStepPhaseMs(bool run, bool diagonal, int &phaseStep) const
{
	phaseStep = 1;
	if (!feedShown() || vrOn())
	{
		return 0;
	}
	// (the eye's pace on each axis, voxels a second; none yet (the first step, a tile move): the walking pace that way)
	double alongX = std::abs(_velocityX), alongY = std::abs(_velocityY);
	const double pace = std::clamp(Options::fpsWalkSpeed, 1, 8) * 16.0 * (run ? SprintPace : 1.0);
	if (alongX + alongY < pace * 0.5)
	{
		alongX = diagonal ? pace * 0.7071 : pace;
		alongY = diagonal ? pace * 0.7071 : 0.0;
	}
	const double untilNext = 16.0 * 1000.0 / (diagonal ? std::max(alongX, alongY) : alongX + alongY) * 0.85; // (a little to spare)
	const double frame = std::max(1.0, _feed->frameMs);
	const int phases = diagonal ? 16 : 8;
	const int phaseTicks = std::clamp((int)std::floor(untilNext / frame) - FeedStepTicks, 2, phases);
	phaseStep = (phases + phaseTicks - 1) / phaseTicks;
	const int ticks = FeedStepTicks + (phases + phaseStep - 1) / phaseStep;
	const int frames = std::clamp((int)std::floor(untilNext / (ticks * frame)), 1, std::max(1, (int)std::lround(Options::battleXcomSpeed / frame)));
	return std::max(1, (int)std::lround((frames - 0.5) * frame)); // (the battle's timer then ticks every so many frames)
}

/**
 * The Live command feed leaves the screen (the full 3D view, or its setting went off): the game's panel back, the map's
 * camera on the whole screen again around what it looked at, the map hidden again under the 3D view,
 * the crosshair back in the middle, and the feed's windows let go (made again when it comes back).
 */
void FirstPersonMode::leaveFeed()
{
	CommandFeed &feed = *_feed;
	restoreHudPanel();
	feed.hud = false;
	feed.hudShown = 0.0;
	feed.cameraIcons = false;
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
	if (feed.projectileHeld)
	{
		_state->_map->setFollowProjectile(true); // (DYNAMIC off held it: the map goes after shots again)
		feed.projectileHeld = false;
	}
	feed.held = false;
	_state->_mouseOverIcons = false; // (the map's cursor back, see handleFeedMouse)
	_mouseLook = isActive() && !_classicHud; // (as setActive has it out of the feed)
	_state->_map->invalidate();
	_state->_map->setVisible(!isActive());
	_state->_warning->setX(feed.warningX);
	_state->_warning->setY(isActive() && !_classicHud ? warningY() : _warningY);
	_crosshair->setX(canvasWidth() / 2 - CrosshairSize / 2);
	_crosshair->setY(canvasHeight() / 2 - CrosshairSize / 2);
	feed.minimap.reset();
	feed.minimapWindow = SDL_Rect{};
	releaseFeedItem(); // (an item picked up goes back where it was)
	feed.inventoryGrid.reset();
	feed.inventoryUnit = nullptr;
	feed.inventoryKey.clear();
	feed.maximised = CommandFeed::MAX_NONE;
	feed.warningKnown = false;
	feed.width = 0; // (laid out again when it comes back)
	feed.moving = -1;
	feed.gliding = false;
	feed.squadDragging = false;
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
 * The feed's buttons for this frame, in the windows' title lines (filled from the right): the drone's DYNAMIC, FOLLOW, FLY TO,
 * stabiliser, night vision (realistic, then enhanced), thermal, names, boxes (not with ISO up), ISO and all over the screen,
 * and on its picture
 * the arrows at its edges (held: it flies that way) and the level buttons at its right; the minimap's all over the screen; the 3D window's, which goes to the full 3D
 * view; the top left window's tabs (the squad, the inventory) and its button which opens the game's inventory screen.
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
	add(CommandFeed::FB_ISO, drone, right, _state->tr("STR_FPS_FEED_ISO"), feed.iso);
	if (feed.iso)
	{
		add(CommandFeed::FB_HUD, drone, right, _state->tr("STR_FPS_FEED_HUD"), feed.hud);
	}
	if (!feed.iso)
	{
		// (the drone's own switches; with ISO the map is the game's, its camera the battle's)
		add(CommandFeed::FB_BOXES, drone, right, _state->tr("STR_FPS_FEED_BOXES"), feed.boxes);
		add(CommandFeed::FB_NAMES, drone, right, _state->tr("STR_FPS_FEED_NAMES"), feed.names);
		add(CommandFeed::FB_THERMAL, drone, right, _state->tr("STR_FPS_FEED_THERMAL"), feed.vision == CommandFeed::VISION_THERMAL);
		add(CommandFeed::FB_NIGHT, drone, right, _state->tr(feed.vision == CommandFeed::VISION_NIGHT_PLUS ? "STR_FPS_FEED_NIGHT_PLUS" : "STR_FPS_FEED_NIGHT"),
			feed.vision == CommandFeed::VISION_NIGHT || feed.vision == CommandFeed::VISION_NIGHT_PLUS);
		add(CommandFeed::FB_STABILISER, drone, right, _state->tr("STR_FPS_FEED_STABILISER"), feed.stabiliser);
		add(CommandFeed::FB_FLY_TO, drone, right, _state->tr("STR_FPS_FEED_FLY_TO"), feed.gliding);
		add(CommandFeed::FB_FOLLOW, drone, right, _state->tr("STR_FPS_FEED_FOLLOW"), feed.follow);
		add(CommandFeed::FB_DYNAMIC, drone, right, _state->tr("STR_FPS_FEED_DYNAMIC"), feed.dynamic);
	}
	// (the arrows at the drone picture's edges, and the level buttons at its right under the top one)
	const SDL_Rect &picture = feed.dronePictureNow();
	const int along = FeedArrowLong * ui, deep = FeedArrowDeep * ui, arrowInset = FeedArrowInset * ui, levelSide = FeedLevelButton * ui;
	const int middleX = picture.x + picture.w / 2, middleY = picture.y + picture.h / 2;
	feed.buttons.push_back({ CommandFeed::FB_MOVE_UP, makeRect(middleX - along / 2, picture.y + arrowInset, along, deep), std::string(), 0, false });
	// (the bottom one above the game's panel while HUD shows it: the panel's buttons are under its place)
	const int panelUp = feed.hudShown > 0.0 ? _state->_map->getIconHeight() : 0;
	feed.buttons.push_back({ CommandFeed::FB_MOVE_DOWN, makeRect(middleX - along / 2, picture.y + picture.h - panelUp - arrowInset - deep, along, deep), std::string(), 1, false });
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
	// (the top left window's tabs, at the left of its title line, short where their words don't fit before its button:
	// at 320x200 the window is narrow)
	int tabLeft = feed.inventory.x + border + 2 * ui;
	const CommandFeed::ButtonKind tabs[] = { CommandFeed::FB_TAB_SQUAD, CommandFeed::FB_TAB_INVENTORY, CommandFeed::FB_TAB_HITLOG };
	auto tabWords = [&](CommandFeed::ButtonKind tab, bool shortWords) {
		const char *id = tab == CommandFeed::FB_TAB_SQUAD ? "STR_FPS_FEED_SQUAD" : tab == CommandFeed::FB_TAB_INVENTORY ? "STR_FPS_FEED_INVENTORY" : "STR_FPS_FEED_HIT_LOG";
		return std::string(_state->tr(shortWords ? std::string(id) + "_SHORT" : std::string(id)));
	};
	auto tabWide = [&](const std::string &words) {
		feed.writer->setText(words);
		return std::max(side, feed.writer->getTextWidth() * ui + 2 * FeedButtonPad * ui);
	};
	int allWide = 0;
	for (const CommandFeed::ButtonKind tab : tabs) allWide += tabWide(tabWords(tab, false)) + inset;
	const bool shortTabs = tabLeft + allWide > inventoryRight - 2 * ui;
	for (const CommandFeed::ButtonKind tab : tabs)
	{
		const std::string words = tabWords(tab, shortTabs);
		const int wide = tabWide(words);
		const bool lit = (tab == CommandFeed::FB_TAB_SQUAD && feed.tab == CommandFeed::TAB_SQUAD) || (tab == CommandFeed::FB_TAB_INVENTORY && feed.tab == CommandFeed::TAB_INVENTORY)
			|| (tab == CommandFeed::FB_TAB_HITLOG && feed.tab == CommandFeed::TAB_HITLOG);
		feed.buttons.push_back({ tab, makeRect(tabLeft, feed.inventory.y + border + inset, wide, side), words, 0, lit });
		tabLeft += wide + inset;
	}
}

/**
 * HUD: the game's panel in the drone's window, at the foot of its picture and in its middle across, as far up as it has
 * risen (see updateFeed); the special actions' buttons the game keeps at the screen's top right (launch, psi, ...) at
 * the picture's top right, left of the level buttons. The battle's surfaces as the game placed them are noted the first
 * time (see restoreHudPanel).
 */
void FirstPersonMode::placeHudPanel()
{
	CommandFeed &feed = *_feed;
	if (feed.hudPlaces.empty())
	{
		for (Surface *surface : _state->_surfaces)
		{
			if (surface == _state->_map || surface == _state->_warning || feedEdgeSurface(surface) || _ownSurfaces.count(surface)) continue;
			feed.hudPlaces.push_back({ surface, { surface->getX(), surface->getY() } });
		}
	}
	const SDL_Rect &picture = feed.dronePictureNow();
	const int width = Options::baseXResolution, height = Options::baseYResolution;
	const double eased = feed.hudShown * feed.hudShown * (3.0 - 2.0 * feed.hudShown);
	const int byX = picture.x + picture.w / 2 - width / 2;
	const int byY = (picture.y + picture.h - height) + (int)std::lround((1.0 - eased) * (_state->_map->getIconHeight() + 2 * feed.ui));
	const int topRight = picture.x + picture.w - (FeedArrowInset + FeedLevelButton + 2) * feed.ui;
	for (const auto &place : feed.hudPlaces)
	{
		Surface *surface = place.first;
		const bool corner = surface == _state->_btnLaunch || surface == _state->_btnPsi || surface == _state->_btnSpecial || surface == _state->_btnSkills;
		if (corner)
		{
			surface->setX(topRight - (width - place.second.first));
			surface->setY(picture.y + place.second.second + FeedArrowInset * feed.ui);
		}
		else
		{
			surface->setX(place.second.first + byX);
			surface->setY(place.second.second + byY);
		}
	}
}

/**
 * HUD gone: the battle's surfaces back where the game had placed them (see placeHudPanel).
 */
void FirstPersonMode::restoreHudPanel()
{
	CommandFeed &feed = *_feed;
	for (const auto &place : feed.hudPlaces)
	{
		place.first->setX(place.second.first);
		place.first->setY(place.second.second);
	}
	feed.hudPlaces.clear();
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
	const SDL_Rect &picture = feed.dronePictureNow();
	const bool panelUp = feed.hudShown > 0.0;
	hideGamePanel(!panelUp);
	if (panelUp)
	{
		placeHudPanel();
	}
	else
	{
		restoreHudPanel();
	}
	WarningMessage *warning = _state->_warning;
	warning->setX(picture.x + (picture.w - warning->getWidth()) / 2);
	warning->setY((panelUp ? (int)_state->_icons->getY() : picture.y + picture.h) - warning->getHeight() - 4 * feed.ui);
	const bool icons = feed.iso && feed.hud;
	if (!sameRect(picture, feed.cameraWindow) || icons != feed.cameraIcons)
	{
		Camera *camera = _state->_map->getCamera();
		const Position centre = camera->getCenterPosition();
		camera->setViewWindow(picture.x, picture.y, picture.w, picture.h, icons);
		camera->centerOnPosition(centre, false);
		feed.cameraWindow = picture;
		feed.cameraIcons = icons;
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
		feed.inventoryDrawn = 0;
	}
	Inventory &grid = *feed.inventoryGrid;
	// (made again only at the inventory's pace or when it changed: see FeedInventoryEveryMs)
	const Uint32 now = SDL_GetTicks();
	const bool due = unit != feed.inventoryUnit || grid.getSelectedItem() || feed.inventoryTouched || feed.inventoryDrawn == 0
		|| now - feed.inventoryDrawn >= FeedInventoryEveryMs;
	// (its own animation: in its time unit mode it steps the battle's animation, which the map steps already)
	auto thinkGrid = [&grid]() {
		grid.setTuMode(false);
		grid.think();
		grid.setTuMode(true);
	};
	if (!due)
	{
		if (unit) thinkGrid();
		return;
	}
	feed.inventoryDrawn = std::max<Uint32>(1, now);
	feed.inventoryTouched = false;
	if (unit != feed.inventoryUnit)
	{
		// (another soldier, or none: the grid on them first (a fallen soldier's items aren't drawn: their tile is gone),
		// then an item held goes back where it was)
		grid.setSelectedUnit(unit, unit != nullptr);
		if (grid.getSelectedItem()) grid.setSelectedItem(nullptr);
		feed.inventoryUnit = unit;
		feed.inventoryKey.clear();
		if (unit) drawFeedDoll(unit);
	}
	// (an item held when its soldier walked off: back, the picture of the ground it came from no longer theirs)
	if (unit && grid.getSelectedItem() && unit->getPosition() != feed.inventoryAt)
	{
		grid.setSelectedItem(nullptr);
	}
	// (an item held that is no longer in the soldier's inventory nor on their tile, or in no slot: loaded into a gun,
	// thrown or gone off elsewhere (the game's inventory screen, the 3D window): let go, not dropped back from nowhere)
	if (unit && grid.getSelectedItem())
	{
		const BattleItem *held = grid.getSelectedItem();
		if (!held->getSlot() || (held->getOwner() != unit && held->getTile() != unit->getTile())) grid.setSelectedItem(nullptr);
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
			feed.inventoryAt = at;
			grid.setSelectedUnit(unit, false);
			if (!first)
			{
				grid.arrangeGround(0); // (the ground's items in their places again: they or the soldier moved)
				_state->updateSoldierInfo(); // (moved in the window: the panel's hands and time units again)
			}
		}
	}
	if (unit) thinkGrid();

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
	// (not for a soldier down or gone from the map: their items have no tile to go to; see the death camera, which
	// keeps the window on them a moment)
	if (!feed.inventoryGrid || !feed.inventoryUnit || !canAct() || !inside(feed.inventoryShown, x, y) || feed.inventoryColumns.empty()
		|| !feed.inventoryUnit->getTile() || feed.inventoryUnit->isOut())
	{
		return;
	}
	const int pictureX = feed.inventoryColumns[x - feed.inventoryShown.x], pictureY = feed.inventoryRows[y - feed.inventoryShown.y];
	feed.inventoryTouched = true; // (its picture made again next frame)
	const SDL_Event &details = *action->getDetails();
	// A right click: the inventory's only to put an item held back. Its own screen closes itself with one (Inventory pops
	// the state on top, which here is the battle: a player's habit would have ended it), so it never gets one here.
	if ((details.type == SDL_MOUSEBUTTONDOWN || details.type == SDL_MOUSEBUTTONUP) && details.button.button == SDL_BUTTON_RIGHT)
	{
		if (details.type == SDL_MOUSEBUTTONDOWN && feed.inventoryGrid->getSelectedItem()) releaseFeedItem();
		return;
	}
	// The buttons the background draws, as on the inventory's screen (a left press): the soldier before or the next
	// (selected for the 3D window too), unload the weapon held, more of the ground (Shift: back).
	if (details.type == SDL_MOUSEBUTTONDOWN)
	{
		_state->_battleGame->cancelAllActions(); // (as the game's inventory button does first)
	}
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
	// (HUD: the game's panel and its buttons too, in the drone's window, see blitFeed)
	return surface == _state->_warning || (_feed->hudShown > 0.0 && surface != _state->_map && !feedEdgeSurface(surface));
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
	if (feed.iso && !_state->_map->showsHiddenMovement())
	{
		// (ISO: the map as the isometric map draws it, in its own colours, still. Not the game's hidden movement: its box
		// stands in the screen's middle, and the window showed a corner of it (the owner, play-testing: "It should just
		// say hidden movement like drone feed does"): the drone's own screen, below)
		SDL_SetClipRect(screen, &picture);
		_state->_map->blit(screen);
		SDL_SetClipRect(screen, nullptr);
		return;
	}
	if (!feed.stabiliser && !feed.iso)
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
 * All of the feed is drawn in the palette's colours (its windows, words, the drone's filters on the map's pixels). The
 * game's screen is in full colour with the engine's OpenGL output, its 32-bit scalers, or a display opened in full
 * colour for True colour (FirstPerson::wantsFullColorDisplay): there the feed is drawn onto a screen of its own in the
 * palette, as the game's would be, and that onto the game's in one go (a player's report from a Mac: the windows were
 * all blue, the palette's numbers taken for a blue, and the drone in colour).
 * @param gameScreen The game's screen.
 */
void FirstPersonMode::blitFeed(SDL_Surface *gameScreen)
{
	CommandFeed &feed = *_feed;
	SDL_Surface *screen = gameScreen;
	if (gameScreen->format->BytesPerPixel != 1)
	{
		if (!feed.paletteScreen || feed.paletteScreen->getWidth() != gameScreen->w || feed.paletteScreen->getHeight() != gameScreen->h)
		{
			feed.paletteScreen.reset(new Surface(gameScreen->w, gameScreen->h, 0, 0));
			SDL_SetColorKey(feed.paletteScreen->getSurface(), 0, 0); // (all of it goes over: colour 0 too, as on the game's 8-bit screen)
		}
		feed.paletteScreen->setPalette(_game->getScreen()->getPalette()); // (the colours the game's screen shows)
		screen = feed.paletteScreen->getSurface();
		SDL_FillRect(screen, nullptr, 0); // (as the game clears its own screen each frame: the gaps between the windows)
	}
	blitFeedUnder(screen);
	// (with HUD the game's panel too, within the drone's picture: nothing of it over the other windows)
	if (feed.hudShown > 0.0)
	{
		SDL_Rect picture = feed.dronePictureNow();
		SDL_SetClipRect(screen, &picture);
	}
	for (Surface *surface : _state->_surfaces)
	{
		if (!_ownSurfaces.count(surface) && feedDrawsGameSurface(surface)) surface->blit(screen);
	}
	SDL_SetClipRect(screen, nullptr);
	blitFeedOver(screen);
	_feedOverlays = true;
	drawOverlays(screen, false, false);
	_feedOverlays = false;
	if (screen != gameScreen)
	{
		SDL_BlitSurface(screen, nullptr, gameScreen, nullptr);
	}
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
	// (a title's room from a point: up to the first button right of it in its line)
	auto roomFrom = [&](const SDL_Rect &frame, int x) {
		int room = frame.x + frame.w - border - x;
		for (const CommandFeed::Button &button : feed.buttons)
		{
			if (button.box.y == frame.y + border + FeedButtonInset * ui && button.box.x >= x && button.box.x < frame.x + frame.w) room = std::min(room, button.box.x - x - 2 * ui);
		}
		return room;
	};
	// (a title's words: after the live dot, up to the first button)
	auto titleWords = [&](const SDL_Rect &frame, const std::string &words, Uint8 colour, bool dot) {
		int x = frame.x + border + 2 * ui;
		if (dot)
		{
			if (blink) fill(screen, x, frame.y + border + titleH / 2 - 2 * ui, 4 * ui, 4 * ui, feed.liveColour);
			x += 6 * ui;
		}
		return x + feedWrite(screen, words, x, frame.y + wordsY, colour, std::max(1, roomFrom(frame, x)));
	};
	// (more of a title, a little after where its words ended, in a colour of its own: whole or not at all, a turn or a
	// time cut short tells nothing)
	auto titleAfter = [&](const SDL_Rect &frame, int after, const std::string &words, Uint8 colour) {
		const int x = after + 6 * ui, room = roomFrom(frame, x);
		feed.writer->setText(words);
		return feed.writer->getTextWidth() * ui <= room ? x + feedWrite(screen, words, x, frame.y + wordsY, colour, room) : after;
	};

	// (a title line's button: its words or sign (all over the screen: a square; back: a square in a square), lit when on)
	auto drawButton = [&](const CommandFeed::Button &button) {
		if (button.kind == CommandFeed::FB_ISO || button.kind == CommandFeed::FB_HUD)
		{
			// (ISO emerald, HUD purple, to stand out; brighter while up, or under the mouse)
			const bool over = inside(button.box, feed.mouseX, feed.mouseY), iso = button.kind == CommandFeed::FB_ISO;
			fill(screen, button.box.x, button.box.y, button.box.w, button.box.h, button.lit || over ? (iso ? feed.emeraldLit : feed.purpleLit) : (iso ? feed.emerald : feed.purple));
			outline(screen, button.box, ui, button.lit ? feed.wordsColour : (iso ? feed.emeraldLit : feed.purpleLit));
			feed.writer->setText(button.words);
			feedWrite(screen, button.words, button.box.x + (button.box.w - feed.writer->getTextWidth() * ui) / 2, button.box.y + (button.box.h - FeedWordsH * ui) / 2 + ui, feed.lowWords);
			return;
		}
		if (moveArrow(button.kind))
		{
			// (discreet: a triangle alone, lit under the mouse or held)
			const bool over = inside(button.box, feed.mouseX, feed.mouseY) || feed.moving == button.kind;
			triangle(screen, button.box, button.value, over ? feed.frameLit : feed.frameColour);
			return;
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
			return;
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
	};

	if (feed.maximised == CommandFeed::MAX_NONE)
	{
		// The window at the top left: its tabs (see feedButtons), then the squad's table titled with how many are up, the
		// inventory's screen scaled into it, titled with the soldier's time units, or the hit log titled with the turn.
		drawFeedWindow(screen, feed.inventory, feed.frameColour);
		int afterTabs = feed.inventory.x + border + 2 * ui;
		for (const CommandFeed::Button &button : feed.buttons)
		{
			if (button.kind == CommandFeed::FB_TAB_SQUAD || button.kind == CommandFeed::FB_TAB_INVENTORY || button.kind == CommandFeed::FB_TAB_HITLOG) afterTabs = std::max(afterTabs, button.box.x + (int)button.box.w);
		}
		const BattleUnit *owner = feed.inventoryUnit;
		const SDL_Rect &area = feed.inventoryPicture;
		if (feed.tab == CommandFeed::TAB_SQUAD)
		{
			int up = 0;
			for (const BattleUnit *unit : feed.squadUnits) if (!unit->isOut() && unit->getFaction() == FACTION_PLAYER) ++up;
			titleAfter(feed.inventory, afterTabs, _state->tr("STR_FPS_FEED_SQUAD_UP").arg(up).arg((int)feed.squadUnits.size()), feed.dimColour);
			drawFeedSquad(screen);
		}
		else if (feed.tab == CommandFeed::TAB_HITLOG)
		{
			titleAfter(feed.inventory, afterTabs, _state->tr("STR_FPS_FEED_TURN").arg(_save->getTurn()), feed.dimColour);
			drawFeedHitLog(screen);
		}
		else
		{
		if (owner) titleAfter(feed.inventory, afterTabs, _state->tr("STR_FPS_FEED_TU").arg(owner->getTimeUnits()), feed.wordsColour);
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
			const std::string words = _state->tr(feed.noInventory ? "STR_FPS_FEED_NO_INVENTORY" : "STR_FPS_FEED_NO_SOLDIER");
			feed.writer->setText(words);
			feedWrite(screen, words, area.x + (area.w - feed.writer->getTextWidth() * ui) / 2, area.y + (area.h - FeedWordsH * ui) / 2, feed.dimColour);
		}
		}

		// The 3D window: the 3D view at the window's size, or no signal.
		const bool plays = isActive() && _view->getVisible();
		drawFeedWindow(screen, feed.view, plays && wantsMouseCapture() ? feed.frameLit : feed.frameColour);
		const BattleUnit *looked = feedLookedUnit();
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
			SDL_Rect from = makeRect(0, 0, std::min(_view->getWidth(), (int)feed.viewPicture.w), std::min(_view->getHeight(), (int)feed.viewPicture.h));
			SDL_BlitSurface(_view->getSurface(), &from, screen, &where); // (no further than its window: a frame after the layout changed)
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

	// The drone's frame round its picture (the game's panel at its foot) and its title (with ISO: the isometric map, without
	// the drone camera's turn and clock).
	{
		const SDL_Rect &drone = feed.droneFrame();
		outline(screen, makeRect(drone.x, drone.y, drone.w, drone.h + border), border, feed.frameColour); // (no foot: the panel is there)
		fill(screen, drone.x + border, drone.y + border, drone.w - 2 * border, titleH, feed.titleBack);
		char clock[16] = "";
		const std::time_t seconds = std::time(nullptr);
		if (const std::tm *local = std::localtime(&seconds)) std::strftime(clock, sizeof(clock), "%H:%M:%S", local);
		titleWords(drone, _state->tr(feed.iso ? "STR_FPS_FEED_ISO_VIEW" : "STR_FPS_FEED_DRONE"), feed.wordsColour, !feed.iso);
		// (the turn and the clock on the picture at its top left, as a camera writes them on its own picture, each in its own
		// colour, on a dark ground that keeps them read over any map; still, as the camera's words are, when the drone floats)
		const SDL_Rect &picture = feed.dronePictureNow();
		if (!feed.iso)
		{
			const std::string turn = _state->tr("STR_FPS_FEED_TURN").arg(_save->getTurn());
			feed.writer->setText(turn);
			const int turnW = feed.writer->getTextWidth() * ui;
			feed.writer->setText(clock);
			const int clockW = feed.writer->getTextWidth() * ui, gap = 6 * ui;
			const int x = picture.x + 5 * ui, y = picture.y + 5 * ui;
			fill(screen, x - 2 * ui, y - 2 * ui, std::min(turnW + gap + clockW + 4 * ui, (int)picture.w - 6 * ui), FeedWordsH * ui + 3 * ui, feed.titleBack);
			feedWrite(screen, turn, x, y, feed.turnColour);
			feedWrite(screen, clock, x + turnW + gap, y, feed.clockColour);
		}
	}

	// The minimap.
	if (feed.maximised == CommandFeed::MAX_NONE && feed.map.w > 0)
	{
		drawFeedWindow(screen, feed.map, feed.frameColour);
		titleWords(feed.map, _state->tr("STR_FPS_FEED_MAP"), feed.wordsColour, false);
		if (feed.minimap) feed.minimap->blitOnto(screen);
	}

	// The title lines' buttons.
	for (const CommandFeed::Button &button : feed.buttons)
	{
		if (feed.maximised != CommandFeed::MAX_NONE && !droneButton(button.kind)) continue;
		drawButton(button);
	}

	// The battle's buttons at the screen's edges, over the windows.
	for (Surface *surface : _state->_surfaces)
	{
		if (feedEdgeSurface(surface)) surface->blit(screen);
	}
}

/**
 * A key while the feed shows: the options key (Escape) opens the battle's options, as on the isometric map (the owner:
 * "Even in feed esc should go to options menu"; its button, on the game's panel, is hidden in the feed), letting go of
 * the mouse the 3D window has first. A cancel key of its own (bound apart from the options key) puts the drone's
 * picture all over the screen back, or lets go of that mouse. Both a press and its release are then the feed's. The rest
 * are the battle's and the 3D view's.
 * @param action The key.
 * @return True if it was the feed's.
 */
bool FirstPersonMode::handleFeedKey(Action *action)
{
	const SDLKey key = action->getDetails()->key.keysym.sym;
	if (!feedShown() || key == SDLK_UNKNOWN || (key != Options::keyBattleOptions && key != Options::keyCancel))
	{
		return false;
	}
	const bool down = action->getDetails()->type == SDL_KEYDOWN;
	if (key == Options::keyBattleOptions)
	{
		if (down)
		{
			if (_mouseLook) setMouseLook(false);
			_state->btnHelpClick(action);
		}
		return true;
	}
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
	if (feed.squadDragging)
	{
		// (the squad table's thumb follows the mouse wherever it goes until the button comes up)
		if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) feed.squadDragging = false;
		else if (event.type == SDL_MOUSEMOTION) feedSquadMouse(x, y, 0, false);
		return true;
	}
	const bool onDrone = inside(feed.dronePictureNow(), x, y);
	// (HUD: the game's panel in the drone's picture is the panel's, as on the isometric map)
	const bool overIcons = feed.hudShown > 0.0 && _state->_icons
		&& inside(makeRect(_state->_icons->getX(), _state->_icons->getY(), _state->_icons->getWidth(), _state->_icons->getHeight()), x, y);
	if (feed.pressedOnDrone)
	{
		// (the map's until the button comes up; let go off the drone's picture, not an order on a tile hidden there: the
		// game takes the mouse to be over its panel, see below)
		if (_state->_mouseOverIcons != (!onDrone || overIcons))
		{
			_state->_mouseOverIcons = !onDrone || overIcons;
			_state->_map->invalidate();
		}
		if (event.type == SDL_MOUSEBUTTONUP) feed.pressedOnDrone = false;
		toMap();
		return false;
	}
	// The map's cursor shows only with the mouse on the drone's picture, off its buttons: elsewhere the game is told the
	// mouse is over its panel, as on the isometric map, which hides the cursor (and keeps its clicks off the map).
	bool onButton = false;
	for (const CommandFeed::Button &each : feed.buttons)
	{
		if ((feed.maximised == CommandFeed::MAX_NONE || droneButton(each.kind)) && inside(each.box, x, y)) onButton = true;
	}
	const bool offMap = !onDrone || onButton || overIcons;
	if (_state->_mouseOverIcons != offMap)
	{
		_state->_mouseOverIcons = offMap;
		_state->_map->invalidate();
	}
	if (!onDrone && feed.overDrone)
	{
		_state->_map->getCamera()->stopMouseScrolling(); // (it scrolled at the window's edge: the mouse left it)
	}
	feed.overDrone = onDrone;

	for (Surface *surface : _state->_surfaces)
	{
		// (OXCE's touch buttons: the debug words' line, over the top left window's title, takes no clicks)
		if (feedEdgeSurface(surface) && surface != _state->_txtDebug && surface->getVisible() && x >= surface->getX() && y >= surface->getY() && x < surface->getX() + surface->getWidth() && y < surface->getY() + surface->getHeight())
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
			if (feed.tab == CommandFeed::TAB_SQUAD) feedSquadMouse(x, y, wheel, leftPress);
			else if (feed.tab == CommandFeed::TAB_HITLOG) feed.hitScroll = std::max(0, feed.hitScroll - wheel * 3); // (the wheel up back through it, see drawFeedHitLog)
			else if (inside(feed.inventoryPicture, x, y) && wheel == 0) feedInventoryMouse(action, x, y);
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
	const BattleUnit *unit = feedLookedUnit();
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
	case CommandFeed::FB_DYNAMIC: feed.dynamic = !feed.dynamic; break; // (off: held from the next frame, see beforeBattleThink)
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
	case CommandFeed::FB_ISO:
		// (the isometric map in the drone's window, or the drone's feed again, without the game's panel)
		feed.iso = !feed.iso;
		if (!feed.iso) feed.hud = false;
		feed.gliding = false;
		_state->_map->invalidate();
		break;
	case CommandFeed::FB_HUD: feed.hud = !feed.hud; break; // (rises or sinks, see updateFeed)
	case CommandFeed::FB_MAP_MAX: showTacticalMap(); break;
	case CommandFeed::FB_VIEW_MAX: toggleView(); return; // (to the full 3D view: the feed is let go of)
	case CommandFeed::FB_TAB_SQUAD: case CommandFeed::FB_TAB_INVENTORY: case CommandFeed::FB_TAB_HITLOG:
		releaseFeedItem();
		feed.tab = button.kind == CommandFeed::FB_TAB_SQUAD ? CommandFeed::TAB_SQUAD : button.kind == CommandFeed::FB_TAB_INVENTORY ? CommandFeed::TAB_INVENTORY
			: CommandFeed::TAB_HITLOG;
		feed.hitScroll = 0;
		feed.inventoryDrawn = 0; // (made again at once: the soldier's items may have changed meanwhile)
		feed.squadDragging = false;
		break;
	case CommandFeed::FB_INVENTORY:
		releaseFeedItem(); // (an item held goes back first)
		_state->btnInventoryClick(action); // (the game's inventory screen)
		return;
	}
	feedButtons();
}


/**
 * The soldier the feed looks through (the 3D window's title, FLY TO, FOLLOW): the one the 3D view is with; else the one
 * selected in our turn, the last we had in the aliens' (never the alien acting: that would give it away).
 * @return The soldier, or null.
 */
BattleUnit *FirstPersonMode::feedLookedUnit() const
{
	if (isActive() && _trackedUnit)
	{
		return _trackedUnit;
	}
	BattleUnit *unit = _save->getSide() == FACTION_PLAYER ? _save->getSelectedUnit() : _lastPlayerUnit;
	return unit && unit->getFaction() == FACTION_PLAYER ? unit : nullptr;
}

/**
 * An item held in the inventory window goes back where it was (its soldier fallen: the grid lets go of them first, their
 * items aren't drawn without their tile).
 */
void FirstPersonMode::releaseFeedItem()
{
	CommandFeed &feed = *_feed;
	if (!feed.inventoryGrid)
	{
		return;
	}
	Inventory &grid = *feed.inventoryGrid;
	if (grid.getSelectedUnit() && !grid.getSelectedUnit()->getTile())
	{
		grid.setSelectedUnit(nullptr, false);
		feed.inventoryUnit = nullptr;
	}
	if (grid.getSelectedItem())
	{
		grid.setSelectedItem(nullptr);
	}
}

/**
 * Each frame, the squad tab's table (see drawFeedSquad): the whole squad as the squad buttons number it (the fallen too),
 * how many rows show under the column heads, the scroll bar when they don't all fit (its thumb as big as the share
 * shown), and the selected soldier's row kept in sight when another is selected.
 */
void FirstPersonMode::updateFeedSquad()
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, rowH = FeedRowH * ui;
	feed.squadUnits = squadList();
	const SDL_Rect &area = feed.inventoryPicture;
	const int total = (int)feed.squadUnits.size();
	const int rowsTop = area.y + rowH + ui, rowsHigh = std::max(0, area.y + (int)area.h - rowsTop);
	feed.squadShown = std::max(1, rowsHigh / rowH);
	const bool scrolls = total > feed.squadShown;
	const int barW = scrolls ? FeedScrollW * ui : 0;
	feed.squadRows = makeRect(area.x, rowsTop, area.w - barW - (scrolls ? ui : 0), rowsHigh);
	const BattleUnit *selected = _save->getSelectedUnit();
	if (selected != feed.squadSeen)
	{
		feed.squadSeen = selected;
		for (int row = 0; row < total; ++row)
		{
			if (feed.squadUnits[row] != selected) continue;
			if (row < feed.squadScroll) feed.squadScroll = row;
			else if (row >= feed.squadScroll + feed.squadShown) feed.squadScroll = row - feed.squadShown + 1;
		}
	}
	feed.squadScroll = std::clamp(feed.squadScroll, 0, std::max(0, total - feed.squadShown));
	if (scrolls)
	{
		feed.squadTrack = makeRect(area.x + area.w - barW, rowsTop, barW, rowsHigh);
		const int thumbH = std::min(rowsHigh, std::max(4 * ui, rowsHigh * feed.squadShown / total));
		const int thumbY = rowsTop + (rowsHigh - thumbH) * feed.squadScroll / std::max(1, total - feed.squadShown);
		feed.squadThumb = makeRect(feed.squadTrack.x, thumbY, barW, thumbH);
	}
	else
	{
		feed.squadTrack = feed.squadThumb = SDL_Rect{};
	}
}

/**
 * The squad tab: a table of the whole squad, a row each, under the column heads: the number (as on the squad buttons),
 * the name, the time units left of the most, health (of the most), energy, morale and the state (OK, wounded, bleeding,
 * down, dead, panicking, berserk, turned against us, ours by mind control); the rows apart on grounds in turn, the
 * selected soldier's lit, the fallen dim; each value in its own colour (its head too), and a soldier low on something
 * shows it (the owner: "colour various properties. When soldiers are low on something lets colour that - emphasize
 * somehow!"): under FeedLowShare of its most on an amber ground, under FeedCriticalShare (time units none left) on a red
 * one, as a state that needs help does (bleeding, panic, berserk, turned; down on amber). Columns as wide as their head
 * or widest value; too narrow a window leaves out morale, then energy, then health. The scroll bar at the right when the
 * rows don't all fit.
 * @param screen The game's screen.
 */
void FirstPersonMode::drawFeedSquad(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, rowH = FeedRowH * ui, gap = FeedColumnGap * ui;
	const SDL_Rect &area = feed.inventoryPicture;
	fill(screen, area.x, area.y, area.w, area.h, feed.black);
	if (feed.squadUnits.empty())
	{
		const std::string words = _state->tr("STR_FPS_FEED_NO_SOLDIER");
		feed.writer->setText(words);
		feedWrite(screen, words, area.x + (area.w - feed.writer->getTextWidth() * ui) / 2, area.y + (area.h - FeedWordsH * ui) / 2, feed.dimColour);
		return;
	}
	enum { COL_NUMBER, COL_NAME, COL_TU, COL_HEALTH, COL_ENERGY, COL_MORALE, COL_STATE, COLUMNS };
	struct Row { std::string cell[COLUMNS]; Uint8 colour[COLUMNS]; Uint8 ground[COLUMNS]; bool picked; };
	const BattleUnit *selected = _save->getSelectedUnit();
	// (a value in its column's colour; low, in light words on an amber ground; critical, in white on red)
	auto level = [&](Row &row, int column, int value, int most, Uint8 colour) {
		row.colour[column] = colour;
		row.ground[column] = 0;
		if (most <= 0) return;
		if (value <= 0 || value < most * FeedCriticalShare) { row.colour[column] = feed.wordsColour; row.ground[column] = feed.criticalBack; }
		else if (value < most * FeedLowShare) { row.colour[column] = feed.lowWords; row.ground[column] = feed.lowBack; }
	};
	std::vector<Row> rows;
	rows.reserve(feed.squadUnits.size());
	for (size_t place = 0; place < feed.squadUnits.size(); ++place)
	{
		const BattleUnit *unit = feed.squadUnits[place];
		const UnitStats *most = unit->getBaseStats();
		Row row;
		std::fill(row.ground, row.ground + COLUMNS, (Uint8)0);
		row.picked = unit == selected;
		const bool out = unit->isOut();
		row.cell[COL_NUMBER] = std::to_string(place + 1);
		row.cell[COL_NAME] = unit->getName(_game->getLanguage(), false);
		row.cell[COL_TU] = std::to_string(unit->getTimeUnits()) + "/" + std::to_string(most->tu);
		row.cell[COL_HEALTH] = std::to_string(unit->getHealth()) + "/" + std::to_string(most->health);
		row.cell[COL_ENERGY] = std::to_string(unit->getEnergy());
		row.cell[COL_MORALE] = std::to_string(unit->getMorale());
		const char *state = "STR_FPS_FEED_STATE_OK";
		Uint8 stateColour = feed.hudColour, stateGround = 0;
		if (out && unit->getHealth() <= 0) { state = "STR_FPS_FEED_STATE_DEAD"; stateColour = feed.dimColour; }
		else if (out) { state = "STR_FPS_FEED_STATE_DOWN"; stateColour = feed.lowWords; stateGround = feed.lowBack; }
		else if (unit->getOriginalFaction() == FACTION_PLAYER && unit->getFaction() != FACTION_PLAYER) { state = "STR_FPS_FEED_STATE_TURNED"; stateColour = feed.wordsColour; stateGround = feed.criticalBack; }
		else if (unit->getOriginalFaction() != FACTION_PLAYER) { state = "STR_FPS_FEED_STATE_CONTROLLED"; stateColour = feed.clockColour; }
		else if (unit->getStatus() == STATUS_PANICKING) { state = "STR_FPS_FEED_STATE_PANIC"; stateColour = feed.wordsColour; stateGround = feed.criticalBack; }
		else if (unit->getStatus() == STATUS_BERSERK) { state = "STR_FPS_FEED_STATE_BERSERK"; stateColour = feed.wordsColour; stateGround = feed.criticalBack; }
		else if (unit->getFatalWounds() > 0) { state = "STR_FPS_FEED_STATE_BLEEDING"; stateColour = feed.wordsColour; stateGround = feed.criticalBack; }
		else if (unit->getHealth() < most->health) { state = "STR_FPS_FEED_STATE_WOUNDED"; stateColour = feed.turnColour; }
		row.cell[COL_STATE] = _state->tr(state);
		const Uint8 plain = out ? feed.dimColour : feed.wordsColour;
		row.colour[COL_NUMBER] = row.colour[COL_NAME] = row.picked ? feed.pickedColour : plain;
		if (out)
		{
			row.colour[COL_TU] = row.colour[COL_HEALTH] = row.colour[COL_ENERGY] = row.colour[COL_MORALE] = feed.dimColour; // (the fallen: nothing to tell)
		}
		else
		{
			level(row, COL_TU, unit->getTimeUnits(), most->tu, feed.tuColour);
			level(row, COL_HEALTH, unit->getHealth(), most->health, feed.healthColour);
			level(row, COL_ENERGY, unit->getEnergy(), most->stamina, feed.energyColour);
			level(row, COL_MORALE, unit->getMorale(), 100, feed.moraleColour);
		}
		row.colour[COL_STATE] = stateColour;
		row.ground[COL_STATE] = stateGround;
		rows.push_back(row);
	}
	// The columns: as wide as their head or widest value; morale, energy and health left out (in that order) while the
	// name wouldn't have FeedNameLeast.
	const char *heads[COLUMNS] = { "#", "STR_FPS_FEED_COLUMN_NAME", "STR_FPS_FEED_COLUMN_TU", "STR_FPS_FEED_COLUMN_HEALTH", "STR_FPS_FEED_COLUMN_ENERGY",
		"STR_FPS_FEED_COLUMN_MORALE", "STR_FPS_FEED_COLUMN_STATE" };
	std::string head[COLUMNS];
	int wide[COLUMNS];
	bool shown[COLUMNS];
	auto measure = [&](const std::string &words) { feed.writer->setText(words); return feed.writer->getTextWidth() * ui; };
	for (int column = 0; column < COLUMNS; ++column)
	{
		head[column] = column == COL_NUMBER ? std::string(heads[column]) : std::string(_state->tr(heads[column]));
		wide[column] = measure(head[column]);
		if (column != COL_NAME) for (const Row &row : rows) wide[column] = std::max(wide[column], measure(row.cell[column]));
		shown[column] = true;
	}
	const int room = (int)feed.squadRows.w - 4 * ui;
	auto others = [&]() {
		int sum = 0;
		for (int column = 0; column < COLUMNS; ++column) if (shown[column] && column != COL_NAME) sum += wide[column] + gap;
		return sum;
	};
	for (const int optional : { (int)COL_MORALE, (int)COL_ENERGY, (int)COL_HEALTH })
	{
		if (room - others() >= FeedNameLeast * ui) break;
		shown[optional] = false;
	}
	wide[COL_NAME] = std::max(0, room - others());
	int left[COLUMNS];
	int x = area.x + 2 * ui;
	for (int column = 0; column < COLUMNS; ++column)
	{
		left[column] = x;
		if (shown[column]) x += wide[column] + gap;
	}
	// (numbers to the right of their column, words to the left; a cell's words cut short to its column)
	auto cell = [&](int column, const std::string &words, int y, Uint8 colour) {
		if (!shown[column] || wide[column] <= 0) return;
		const bool number = column == COL_NUMBER || column == COL_TU || column == COL_HEALTH || column == COL_ENERGY || column == COL_MORALE;
		const int at = number ? left[column] + wide[column] - measure(words) : left[column];
		feedWrite(screen, words, at, y, colour, wide[column]);
	};
	// (the heads in their columns' colours; the words in the middle of a row)
	const int wordsAt = (rowH - FeedWordsH * ui) / 2 + ui;
	const Uint8 headColour[COLUMNS] = { feed.dimColour, feed.dimColour, feed.tuColour, feed.healthColour, feed.energyColour, feed.moraleColour, feed.dimColour };
	for (int column = 0; column < COLUMNS; ++column) cell(column, head[column], area.y + wordsAt, headColour[column]);
	fill(screen, area.x, area.y + rowH, (int)feed.squadRows.w, ui, feed.frameColour);
	const int last = std::min((int)rows.size(), feed.squadScroll + feed.squadShown);
	for (int place = feed.squadScroll; place < last; ++place)
	{
		const Row &row = rows[place];
		const int y = feed.squadRows.y + (place - feed.squadScroll) * rowH;
		// (every other row on a ground of its own, the selected one lit; a low value's ground round its cell, apart
		// from the next row's)
		if (row.picked) fill(screen, feed.squadRows.x, y, feed.squadRows.w, rowH, feed.rowBack);
		else if (place % 2) fill(screen, feed.squadRows.x, y, feed.squadRows.w, rowH, feed.titleBack);
		for (int column = 0; column < COLUMNS; ++column)
		{
			if (row.ground[column] && shown[column] && wide[column] > 0) fill(screen, left[column] - ui, y + ui, wide[column] + 2 * ui, rowH - 2 * ui, row.ground[column]);
			cell(column, row.cell[column], y + wordsAt, row.colour[column]);
		}
	}
	// The scroll bar: its track, the thumb lit while dragged or under the mouse.
	if (feed.squadTrack.w > 0)
	{
		fill(screen, feed.squadTrack.x, feed.squadTrack.y, feed.squadTrack.w, feed.squadTrack.h, feed.frameColour);
		const bool lit = feed.squadDragging || inside(feed.squadThumb, feed.mouseX, feed.mouseY);
		fill(screen, feed.squadThumb.x, feed.squadThumb.y, feed.squadThumb.w, feed.squadThumb.h, lit ? feed.frameLit : feed.dimColour);
	}
}

/**
 * The mouse on the squad tab: the wheel scrolls its rows; a left press on the scroll bar's thumb drags it (it follows
 * the mouse until the button comes up, see handleFeedMouse), on its track a page up or down; on a row it selects that
 * soldier, as their squad button does (one who is up and ours, in our turn).
 * @param x, y Where it is on the game's screen.
 * @param wheel The wheel's notch (see wheelStep), 0 for none.
 * @param leftPress True for a left press.
 */
void FirstPersonMode::feedSquadMouse(int x, int y, int wheel, bool leftPress)
{
	CommandFeed &feed = *_feed;
	const int total = (int)feed.squadUnits.size(), most = std::max(0, total - feed.squadShown);
	if (feed.squadDragging)
	{
		const int travel = std::max(1, (int)feed.squadTrack.h - (int)feed.squadThumb.h);
		feed.squadScroll = std::clamp(feed.dragFromScroll + (int)std::lround((y - feed.dragFromY) * (double)most / travel), 0, most);
		return;
	}
	if (wheel != 0)
	{
		feed.squadScroll = std::clamp(feed.squadScroll + wheel, 0, most);
		return;
	}
	if (!leftPress)
	{
		return;
	}
	if (feed.squadThumb.w > 0 && inside(feed.squadThumb, x, y))
	{
		feed.squadDragging = true;
		feed.dragFromY = y;
		feed.dragFromScroll = feed.squadScroll;
		return;
	}
	if (feed.squadTrack.w > 0 && inside(feed.squadTrack, x, y))
	{
		feed.squadScroll = std::clamp(feed.squadScroll + (y < feed.squadThumb.y ? -feed.squadShown : feed.squadShown), 0, most);
		return;
	}
	if (inside(feed.squadRows, x, y))
	{
		const int place = feed.squadScroll + (y - feed.squadRows.y) / (FeedRowH * feed.ui);
		if (place >= 0 && place < total)
		{
			if (TextButton::soundPress) TextButton::soundPress->play(Mix_GroupAvailable(0));
			selectUnit(feed.squadUnits[place]);
		}
	}
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
 * A hit-log entry the game makes (SavedBattleGame::appendToHitLog: its own log keeps only the player's turn), kept for
 * the Live command feed's hit log tab, every turn of the battle, the aliens' too (the owner: "the log should keep all
 * turns"), as the game words it: a turn's start (with the turn's message), a firing (the weapon, then its shots on that
 * line), reaction fire, a shot ("=>"), and each hit's word (0, hit, HIT) with whom it hurt (see FirstPerson::hitLogDetail:
 * in the feed always), its "killed", "down" and "unseen" parts of their own for their colours (see drawFeedHitLog).
 * Nothing without the feed (made once it first showed; it keeps the log while the full 3D view is up).
 * @param state The battlescape screen.
 * @param type The entry (HitLogEntryType).
 * @param faction The side it is for.
 * @param text Its words: the weapon, the turn's message, the hit's detail; else empty.
 */
void FirstPersonMode::hitLogged(BattlescapeState *state, int type, int faction, const std::string &text)
{
	FirstPersonMode *mode = state ? state->_firstPerson : nullptr;
	if (!mode || !mode->_feed)
	{
		return;
	}
	CommandFeed &feed = *mode->_feed;
	std::vector<CommandFeed::HitLine> &lines = feed.hitLines;
	// (the game's words, without the spaces it ends them with)
	auto word = [state](const char *id) {
		std::string words = state->tr(id);
		while (!words.empty() && words.back() == ' ') words.pop_back();
		return words;
	};
	// (the line shots go on: the last firing, reaction fire or shots of this side, else a new line: the player's last
	// firing again, as the game's own log has it after the enemy's reaction fire; else shots alone)
	auto shotLine = [&]() -> CommandFeed::HitLine & {
		if (lines.empty() || lines.back().kind == CommandFeed::HIT_TURN || lines.back().faction != faction)
		{
			if (faction == FACTION_PLAYER && !feed.hitWeapon.empty()) lines.push_back({ { { feed.hitWeapon + ":", CommandFeed::HS_TEXT } }, CommandFeed::HIT_FIRING, faction, false });
			else lines.push_back({ {}, CommandFeed::HIT_SHOTS, faction, false });
		}
		return lines.back();
	};
	switch (type)
	{
	case HITLOG_NEW_TURN:
	case HITLOG_NEW_TURN_WITH_MESSAGE:
	{
		feed.hitWeapon.clear();
		feed.hitScroll = 0;
		std::string header = std::string(state->tr("STR_FPS_FEED_TURN").arg(mode->_save->getTurn())) + "  "
			+ word(faction == FACTION_PLAYER ? "STR_FPS_FEED_SIDE_XCOM" : faction == FACTION_HOSTILE ? "STR_FPS_FEED_SIDE_ALIENS" : "STR_FPS_FEED_SIDE_CIVILIANS");
		if (type == HITLOG_NEW_TURN_WITH_MESSAGE && !text.empty()) header += "  " + text;
		lines.push_back({ { { header, CommandFeed::HS_HEADER } }, CommandFeed::HIT_TURN, faction, false });
		break;
	}
	case HITLOG_PLAYER_FIRING:
		// (a firing chosen again with no shot yet, as the game's own log, says it once)
		if (!lines.empty() && lines.back().kind == CommandFeed::HIT_FIRING && !lines.back().shot) lines.pop_back();
		lines.push_back({ { { text + ":", CommandFeed::HS_TEXT } }, CommandFeed::HIT_FIRING, faction, false });
		feed.hitWeapon = text;
		break;
	case HITLOG_REACTION_FIRE:
		if (lines.empty() || lines.back().kind != CommandFeed::HIT_REACTION || lines.back().shot)
		{
			lines.push_back({ { { word("STR_HIT_LOG_REACTION_FIRE"), CommandFeed::HS_TEXT } }, CommandFeed::HIT_REACTION, faction, false });
		}
		break;
	case HITLOG_NEW_SHOT:
	{
		CommandFeed::HitLine &line = shotLine();
		line.parts.push_back({ word("STR_HIT_LOG_NEW_BULLET"), CommandFeed::HS_TEXT });
		line.shot = true;
		break;
	}
	case HITLOG_NO_DAMAGE:
	case HITLOG_SMALL_DAMAGE:
	case HITLOG_BIG_DAMAGE:
	{
		CommandFeed::HitLine &line = shotLine();
		line.parts.push_back({ word(type == HITLOG_NO_DAMAGE ? "STR_HIT_LOG_NO_DAMAGE" : type == HITLOG_SMALL_DAMAGE ? "STR_HIT_LOG_SMALL_DAMAGE" : "STR_HIT_LOG_BIG_DAMAGE"),
			type == HITLOG_NO_DAMAGE ? CommandFeed::HS_ZERO : type == HITLOG_SMALL_DAMAGE ? CommandFeed::HS_HIT : CommandFeed::HS_BIG_HIT });
		// (whom it hurt, its "killed", "down" or "unseen" a part of its own: the tab has it whatever "Detailed hit log" says)
		std::string detail = text.empty() ? FirstPerson::takeHitDetailForFeed() : text;
		if (!text.empty()) FirstPerson::takeHitDetailForFeed(); // (the same text: not taken again for the next hit)
		while (!detail.empty() && detail.back() == ' ') detail.pop_back();
		const std::pair<std::string, CommandFeed::HitStyle> marks[] = { { word("STR_FPS_HIT_LOG_KILLED"), CommandFeed::HS_KILLED },
			{ word("STR_FPS_HIT_LOG_DOWN"), CommandFeed::HS_DOWN }, { word("STR_FPS_HIT_LOG_UNSEEN"), CommandFeed::HS_UNSEEN } };
		size_t at = std::string::npos, length = 0;
		CommandFeed::HitStyle style = CommandFeed::HS_TEXT;
		for (const auto &mark : marks)
		{
			const size_t found = mark.first.empty() ? std::string::npos : detail.rfind(mark.first);
			if (found != std::string::npos) { at = found; length = mark.first.size(); style = mark.second; break; }
		}
		if (at == std::string::npos)
		{
			if (!detail.empty()) line.parts.push_back({ detail, CommandFeed::HS_TEXT });
		}
		else
		{
			// (glued to the words before and after where the detail has no space between them: "(unseen)", "killed)")
			if (at > 0) line.parts.push_back({ detail.substr(0, at), CommandFeed::HS_TEXT });
			line.parts.push_back({ detail.substr(at, length), style, at > 0 && detail[at - 1] != ' ' });
			if (at + length < detail.size()) line.parts.push_back({ detail.substr(at + length), CommandFeed::HS_TEXT, detail[at + length] != ' ' });
		}
		line.shot = true;
		break;
	}
	default:
		return;
	}
	// (a long battle's oldest lines go first)
	if (lines.size() > 600)
	{
		lines.erase(lines.begin(), lines.begin() + 100);
		++feed.hitErased;
	}
	++feed.hitVersion;
}

/**
 * The hit log tab (the owner: "do not colour whole strings red. The text is gray only hit. Emphasis is red", turns marked
 * "yellow mark for xcom and red mark for AI", a rule between them): the log's lines wrapped into the window's width, the
 * words grey; a hit's word emphasised, "0" dim, "hit" dark red, "HIT" bright red, "killed" bright red, "down" orange,
 * "unseen" dim; each turn's heading in the turn's amber after its side's mark (a square, yellow for X-COM, red for the
 * others), and a rule across the window before it (but the first); each line of shots after a smaller mark of the side
 * that fired (the owner: "mark also our team vs alien shots": the squad green, the enemy red); the newest at the foot, the wheel scrolling back
 * through it (and following the newest again at the foot).
 * @param screen The game's screen.
 */
void FirstPersonMode::drawFeedHitLog(SDL_Surface *screen)
{
	CommandFeed &feed = *_feed;
	const int ui = feed.ui, rowH = (FeedWordsH + 1) * ui, margin = 3 * ui, markSide = (FeedWordsH - 3) * ui, shotMark = 4 * ui, indent = markSide + 3 * ui;
	const SDL_Rect &area = feed.inventoryPicture;
	fill(screen, area.x, area.y, area.w, area.h, feed.black);
	const int width = area.w - 2 * margin - indent;
	auto textWidth = [&](const std::string &words) { feed.writer->setText(words); return feed.writer->getTextWidth() * ui; };
	if (feed.hitRowsVersion != feed.hitVersion || feed.hitRowsWidth != width)
	{
		// (each line in words, as many to a row as fit, each word in its part's style; a word wider than a row alone.
		// Only the last line wrapped before (its shots may have grown) and the lines after it, unless the width changed
		// or the oldest lines went)
		if (feed.hitRowsWidth != width || feed.hitRowsErased != feed.hitErased || feed.hitLineRow.size() > feed.hitLines.size())
		{
			feed.hitRows.clear();
			feed.hitLineRow.clear();
			feed.hitRowsErased = feed.hitErased;
		}
		size_t from = feed.hitLineRow.size();
		if (from > 0)
		{
			--from;
			feed.hitRows.resize(feed.hitLineRow[from]);
			feed.hitLineRow.resize(from);
		}
		for (size_t lineIndex = from; lineIndex < feed.hitLines.size(); ++lineIndex)
		{
			const CommandFeed::HitLine &line = feed.hitLines[lineIndex];
			feed.hitLineRow.push_back(feed.hitRows.size());
			if (line.kind == CommandFeed::HIT_TURN)
			{
				if (!feed.hitRows.empty()) feed.hitRows.push_back({ {}, false, true, line.faction });
				feed.hitRows.push_back({ line.parts, true, false, line.faction });
				continue;
			}
			// (the words of all its parts, a part's first glued to the word before when the part says so: the ")" after
			// "killed"; a group of glued words wraps whole)
			std::vector<std::vector<CommandFeed::HitPart>> groups;
			for (const CommandFeed::HitPart &part : line.parts)
			{
				bool spaced = !part.glued;
				for (size_t at = 0; at < part.text.size();)
				{
					if (part.text[at] == ' ') { spaced = true; ++at; continue; }
					const size_t end = std::min(part.text.find(' ', at), part.text.size());
					CommandFeed::HitPart word = { part.text.substr(at, end - at), part.style };
					if (spaced || groups.empty()) groups.push_back({ word });
					else { word.glued = true; groups.back().push_back(word); }
					spaced = false;
					at = end;
				}
			}
			CommandFeed::HitRow row = { {}, false, false, line.faction, true };
			std::string rowText;
			for (const std::vector<CommandFeed::HitPart> &group : groups)
			{
				std::string groupText;
				for (const CommandFeed::HitPart &word : group) groupText += word.text;
				const std::string longer = rowText.empty() ? groupText : rowText + " " + groupText;
				if (!rowText.empty() && textWidth(longer) > width)
				{
					feed.hitRows.push_back(row);
					row.words.clear();
					row.lineStart = false;
					rowText = groupText;
				}
				else
				{
					rowText = longer;
				}
				row.words.insert(row.words.end(), group.begin(), group.end());
			}
			if (!row.words.empty()) feed.hitRows.push_back(row);
		}
		feed.hitRowsVersion = feed.hitVersion;
		feed.hitRowsWidth = width;
	}
	if (feed.hitRows.empty())
	{
		const std::string words = _state->tr("STR_FPS_FEED_HIT_LOG_EMPTY");
		feedWrite(screen, words, area.x + (area.w - textWidth(words)) / 2, area.y + (area.h - FeedWordsH * ui) / 2, feed.dimColour);
		return;
	}
	auto colourOf = [&](CommandFeed::HitStyle style) {
		switch (style)
		{
		case CommandFeed::HS_HEADER: return feed.turnColour;
		case CommandFeed::HS_ZERO: case CommandFeed::HS_UNSEEN: return feed.dimColour;
		case CommandFeed::HS_HIT: return feed.logHit;
		case CommandFeed::HS_BIG_HIT: case CommandFeed::HS_KILLED: return feed.logBigHit;
		case CommandFeed::HS_DOWN: return feed.logDown;
		default: return feed.logText;
		}
	};
	const int shown = std::max(1, (area.h - 2 * margin) / rowH);
	feed.hitScroll = std::clamp(feed.hitScroll, 0, std::max(0, (int)feed.hitRows.size() - shown));
	const int last = (int)feed.hitRows.size() - feed.hitScroll, first = std::max(0, last - shown);
	const int space = std::max(ui, textWidth("a a") - 2 * textWidth("a"));
	for (int index = first; index < last; ++index)
	{
		const CommandFeed::HitRow &row = feed.hitRows[index];
		const int y = area.y + margin + (index - first) * rowH;
		if (row.rule)
		{
			fill(screen, area.x + margin, y + rowH / 2 - ui / 2, area.w - 2 * margin, ui, feed.frameColour);
			continue;
		}
		if (row.heading)
		{
			fill(screen, area.x + margin, y + ui, markSide, markSide, row.faction == FACTION_PLAYER ? feed.xcomMark : feed.enemyMark);
		}
		else if (row.lineStart)
		{
			// (who fired: the squad green, the enemy red; smaller than a turn's mark)
			fill(screen, area.x + margin + (markSide - shotMark) / 2, y + ui + (markSide - shotMark) / 2, shotMark, shotMark,
				row.faction == FACTION_PLAYER ? feed.hudColour : feed.enemyMark);
		}
		int x = area.x + margin + indent;
		for (size_t at = 0; at < row.words.size() && x < area.x + area.w - margin; ++at)
		{
			const CommandFeed::HitPart &word = row.words[at];
			if (at > 0 && !word.glued) x += space;
			x += feedWrite(screen, word.text, x, y, colourOf(word.style), area.x + area.w - margin - x);
		}
	}
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
	feed.turnColour = bright(250, 185, 60);
	feed.clockColour = bright(110, 215, 255);
	feed.dangerColour = bright(245, 75, 60);
	feed.rowBack = nearestColor(palette, 32, 46, 78);
	feed.tuColour = bright(90, 235, 90);
	feed.healthColour = bright(255, 150, 175);
	feed.energyColour = bright(255, 160, 50);
	feed.moraleColour = bright(195, 150, 255);
	feed.lowBack = nearestColor(palette, 205, 120, 0);
	feed.lowWords = bright(255, 255, 255);
	feed.criticalBack = nearestColor(palette, 200, 20, 20);
	feed.emerald = nearestColor(palette, 20, 140, 90);
	feed.emeraldLit = bright(60, 215, 140);
	feed.logText = bright(170, 172, 180);
	feed.logHit = nearestColor(palette, 185, 45, 35);
	feed.logBigHit = bright(255, 60, 45);
	feed.logDown = bright(255, 150, 40);
	feed.xcomMark = bright(255, 225, 40);
	feed.enemyMark = nearestColor(palette, 230, 30, 30);
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
		if (!feed.warningKnown) feed.warningX = _state->_warning->getX();
		feed.warningKnown = true;
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
	// (each lit pixel of the words a block of the magnification, written straight into the screen within its clip: the
	// feed draws on a screen in the palette, see blitFeed)
	const SDL_Rect &clip = screen->clip_rect;
	const bool direct = screen->format->BytesPerPixel == 1;
	for (int row = 0; row < writer.getHeight(); ++row)
	{
		for (int column = 0; column <= letters && column < writer.getWidth(); ++column)
		{
			const Uint8 pixel = writer.getPixel(column, row);
			if (!pixel) continue;
			if (!direct)
			{
				fill(screen, x + column * ui, y + row * ui, ui, ui, pixel);
				continue;
			}
			for (int down = 0; down < ui; ++down)
			{
				const int screenY = y + row * ui + down;
				if (screenY < clip.y || screenY >= clip.y + (int)clip.h) continue;
				Uint8 *line = (Uint8 *)screen->pixels + (size_t)screenY * screen->pitch;
				for (int across = 0; across < ui; ++across)
				{
					const int screenX = x + column * ui + across;
					if (screenX >= clip.x && screenX < clip.x + (int)clip.w) line[screenX] = pixel;
				}
			}
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
