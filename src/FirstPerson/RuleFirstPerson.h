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
 * RuleFirstPerson.h: the `firstPerson:` section of the rulesets, which the OpenXcom3D mod fills
 * in (mod/OpenXcom3D/OpenXcom3D.rul). The game's Mod class keeps one of these and reads
 * every mod's section into it.
 */
#include <algorithm>
#include <array>
#include <bitset>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../Engine/Yaml.h"

namespace OpenXcom
{

/**
 * The `firstPerson:` ruleset section. The first-person mode is engine code, but it only
 * switches on when an active mod (OpenXcom3D) declares `firstPerson: enabled: true`. The mod
 * also says how far the view reaches, which terrain records are windows (and which of their
 * colours are glass), what shape the parts the game gives no voxels have, what weather each
 * terrain has, which music plays in combat, which armors and items give night vision, and which
 * built-in items are drawn in the hands anyway.
 */
class RuleFirstPerson
{
public:
	/// The window records of one terrain data set, and the sprite colours that are glass.
	struct Windows
	{
		std::set<int> records;  // record numbers in the terrain's MCD data set
		std::bitset<256> glass; // palette colours of those records that are see-through glass
	};
	/// The shape of a terrain record the game gives no voxels (shots and sight pass through it,
	/// though the map draws something there, often something no one can walk into): boxes of
	/// voxels in the tile, each west, north, bottom, east, south, top (x 0..15 west to east, y
	/// 0..15 north to south, z 0..23 up, inclusive), for one game (xcom1, xcom2: the two share
	/// some data set names). A ghost (an object, a wall) is drawn see-through, as the parts
	/// without shape are (see FirstPersonView::buildShape); not a ghost (a floor: the sea), as if
	/// the game had given it those voxels.
	struct Shape
	{
		std::string game;
		std::vector<std::array<int, 6>> boxes;
		bool ghost = true;
	};
	/// How an item's picture is given volume in 3D: worked out (AUTO), rounded (CUSHION) or flat (PLATE: a blade).
	enum HeldShape { HELD_AUTO = -1, HELD_CUSHION, HELD_PLATE };
	/// What a weapon's shots are: worked out (AUTO), a gun's rounds (POWDER) or objects flying (OBJECT: arrows, bolts, harpoons).
	enum ShotKind { SHOT_AUTO = -1, SHOT_POWDER, SHOT_OBJECT };
	/// One item's entry in the item table (`items:`): what the 3D view isn't left to work out.
	struct ItemLook
	{
		HeldShape held = HELD_AUTO; // how its picture is given volume
		ShotKind shot = SHOT_AUTO;  // what its shots are (a weapon's, or an ammo's)
	};
private:
	bool _enabled;
	int _maxViewDistance;                        // in tiles
	std::map<std::string, Windows> _windows;     // terrain data set -> its windows
	std::map<std::pair<std::string, int>, std::vector<Shape>> _shapes; // (terrain data set, record) -> its shapes, one per game
	std::map<std::string, std::string> _weather; // terrain data set -> rain, snow, dust, ...
	std::vector<std::string> _combatMusic;       // tense tracks to try, first one the game has wins
	std::set<std::string> _nightVision;          // armors and items that give their wearer night vision
	std::set<std::string> _undergroundMissions;  // missions underground all the way up, whatever their map's edges show
	std::set<std::string> _builtInItemsShown;    // built-in items drawn in the hands all the same (a shield, an arm cannon)
	std::map<std::string, ItemLook> _itemLooks;  // the item table: item type -> what it's set to
	std::map<std::string, int> _signalLevels;    // mission type -> how badly helmet cameras glitch (1 as usual, 2 a lot)
public:
	/// Creates the rule with the feature switched off.
	RuleFirstPerson() : _enabled(false), _maxViewDistance(60), _combatMusic({ "GMDEFEND", "GMDEFBAS" }) { }

	/// Loads the `firstPerson:` node; later mods override earlier ones (windows add up).
	void load(const YAML::YamlNodeReader& reader)
	{
		// The switch, and how far the view reaches (10 to 200 tiles).
		reader.tryRead("enabled", _enabled);
		reader.tryRead("maxViewDistance", _maxViewDistance);
		_maxViewDistance = std::clamp(_maxViewDistance, 10, 200);

		// Windows: per terrain data set, the record numbers that are windows and their glass colours.
		if (const auto& windows = reader["windows"])
		{
			for (const auto& entry : windows.children())
			{
				std::string dataSet;
				std::vector<int> records, colors;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("records", records);
				entry.tryRead("glassColors", colors);
				Windows &dataSetWindows = _windows[dataSet];
				dataSetWindows.records.insert(records.begin(), records.end());
				for (int color : colors)
				{
					if (color > 0 && color < 256) dataSetWindows.glass.set(color);
				}
			}
		}

		// Shapes: per game and terrain data set, records and the boxes of voxels they're built of
		// (a later mod's shape for the same game and record replaces an earlier one).
		if (const auto& shapes = reader["shapes"])
		{
			for (const auto& entry : shapes.children())
			{
				Shape shape;
				std::string dataSet;
				std::vector<int> records;
				std::vector<std::vector<int>> boxes;
				entry.tryRead("game", shape.game);
				entry.tryRead("ghost", shape.ghost);
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("records", records);
				entry.tryRead("boxes", boxes);
				for (const std::vector<int> &box : boxes)
				{
					if (box.size() != 6) continue;
					shape.boxes.push_back({ std::clamp(box[0], 0, 15), std::clamp(box[1], 0, 15), std::clamp(box[2], 0, 23),
						std::clamp(box[3], 0, 15), std::clamp(box[4], 0, 15), std::clamp(box[5], 0, 23) });
				}
				for (int record : records)
				{
					std::vector<Shape> &recordShapes = _shapes[std::make_pair(dataSet, record)];
					recordShapes.erase(std::remove_if(recordShapes.begin(), recordShapes.end(), [&](const Shape &old) { return old.game == shape.game; }), recordShapes.end());
					recordShapes.push_back(shape);
				}
			}
		}

		// Combat music, what gives night vision, and the built-in items drawn in the hands (a
		// list replaces the earlier one).
		reader.tryRead("combatMusic", _combatMusic);
		std::vector<std::string> nightVision, builtInItemsShown;
		if (reader.tryRead("nightVision", nightVision))
		{
			_nightVision = std::set<std::string>(nightVision.begin(), nightVision.end());
		}
		// Missions underground all the way up (rock round the map and above it), for a map whose
		// rock the 3D view can't tell from its edges (it tells most by itself: see
		// FirstPersonView::chooseGround). Mission types only; a map that starts on the surface
		// must never be listed, nor a name two games share for different maps.
		std::vector<std::string> undergroundMissions;
		if (reader.tryRead("undergroundMissions", undergroundMissions))
		{
			_undergroundMissions = std::set<std::string>(undergroundMissions.begin(), undergroundMissions.end());
		}
		if (reader.tryRead("builtInItemsShown", builtInItemsShown))
		{
			_builtInItemsShown = std::set<std::string>(builtInItemsShown.begin(), builtInItemsShown.end());
		}

		// The item table: groups of item types (as the mods name them), each with what it sets:
		// held (cushion, plate, or auto) and shot (powder, object, or auto). A later entry
		// overrides what it sets for an item, and leaves the rest.
		if (const auto& items = reader["items"])
		{
			for (const auto& entry : items.children())
			{
				std::vector<std::string> types;
				std::string held, shot;
				entry.tryRead("types", types);
				entry.tryRead("held", held);
				entry.tryRead("shot", shot);
				for (const std::string &type : types)
				{
					ItemLook &look = _itemLooks[type];
					if (!held.empty()) look.held = held == "plate" ? HELD_PLATE : held == "cushion" ? HELD_CUSHION : HELD_AUTO;
					if (!shot.empty()) look.shot = shot == "object" ? SHOT_OBJECT : shot == "powder" ? SHOT_POWDER : SHOT_AUTO;
				}
			}
		}

		// The helmet cameras' bad signal by mission (alien deployment) type: groups of missions,
		// each with its level (1 now and then, as on any mission; 2 a lot: the aliens' own ground).
		if (const auto& badSignal = reader["badSignal"])
		{
			for (const auto& entry : badSignal.children())
			{
				std::vector<std::string> missions;
				int level = 1;
				entry.tryRead("missions", missions);
				entry.tryRead("level", level);
				for (const std::string &mission : missions) _signalLevels[mission] = std::clamp(level, 0, 2);
			}
		}

		// Weather: per terrain data set, its kind ("rain", "snow", "dust", ...).
		if (const auto& weather = reader["weather"])
		{
			for (const auto& entry : weather.children())
			{
				std::string dataSet, type;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("type", type);
				_weather[dataSet] = type;
			}
		}
	}

	/// Has a mod switched the first-person mode on?
	bool isEnabled() const { return _enabled; }
	/// How far the view reaches, in tiles.
	int getMaxViewDistance() const { return _maxViewDistance; }
	/// The window records of a terrain data set, or nullptr if it has none.
	const Windows *getWindows(const std::string &dataSet) const
	{
		auto found = _windows.find(dataSet);
		return found != _windows.end() ? &found->second : nullptr;
	}
	/// The shape of a terrain record for a game (xcom1, xcom2), or nullptr if it has none.
	const Shape *getShape(const std::string &dataSet, int record, const std::string &game) const
	{
		auto found = _shapes.find(std::make_pair(dataSet, record));
		if (found == _shapes.end()) return nullptr;
		for (const Shape &shape : found->second)
		{
			if (shape.game == game) return &shape;
		}
		return nullptr;
	}
	/// Tense music tracks for when aliens are in sight (the first one the game has is played).
	const std::vector<std::string> &getCombatMusic() const { return _combatMusic; }
	/// Does wearing this armor, or carrying this item, give night vision?
	bool givesNightVision(const std::string &type) const { return _nightVision.count(type) > 0; }
	/// Is a mission (its alien deployment type) listed as underground all the way up?
	bool isUnderground(const std::string &mission) const { return _undergroundMissions.count(mission) > 0; }
	/// Is a built-in item (fixed to its unit, or its special weapon) drawn in the hands all the same?
	bool showsBuiltInItem(const std::string &type) const { return _builtInItemsShown.count(type) > 0; }
	/// An item's entry in the item table, or nullptr if it has none (the 3D view works it all out).
	const ItemLook *getItemLook(const std::string &type) const
	{
		auto found = _itemLooks.find(type);
		return found != _itemLooks.end() ? &found->second : nullptr;
	}
	/// How badly the helmet cameras glitch on a mission (its alien deployment type): 1 now and then (any mission not listed), 2 a lot, 0 never.
	int getSignalLevel(const std::string &mission) const
	{
		auto found = _signalLevels.find(mission);
		return found != _signalLevels.end() ? found->second : 1;
	}
	/// The weather of a terrain data set ("rain", "snow", "dust", ...), or empty for none.
	std::string getWeather(const std::string &dataSet) const
	{
		auto found = _weather.find(dataSet);
		return found != _weather.end() ? found->second : std::string();
	}
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
