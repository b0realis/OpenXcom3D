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
#include <bitset>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../Engine/Yaml.h"

namespace OpenXcom
{

/**
 * The `firstPerson:` ruleset section. The first-person mode is engine code, but it only
 * switches on when an active mod (OpenXcom3D) declares `firstPerson: enabled: true`. The mod
 * also says how far the view reaches, which terrain records are windows (and which of their
 * colours are glass), what weather each terrain has, which music plays in combat, which armors
 * and items give night vision, and which built-in items are drawn in the hands anyway.
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
private:
	bool _enabled;
	int _maxViewDistance;                        // in tiles
	std::map<std::string, Windows> _windows;     // terrain data set -> its windows
	std::map<std::string, std::string> _weather; // terrain data set -> rain, snow, dust, ...
	std::vector<std::string> _combatMusic;       // tense tracks to try, first one the game has wins
	std::set<std::string> _nightVision;          // armors and items that give their wearer night vision
	std::set<std::string> _builtInItemsShown;    // built-in items drawn in the hands all the same (a shield, an arm cannon)
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

		// Combat music, what gives night vision, and the built-in items drawn in the hands (a
		// list replaces the earlier one).
		reader.tryRead("combatMusic", _combatMusic);
		std::vector<std::string> nightVision, builtInItemsShown;
		if (reader.tryRead("nightVision", nightVision))
		{
			_nightVision = std::set<std::string>(nightVision.begin(), nightVision.end());
		}
		if (reader.tryRead("builtInItemsShown", builtInItemsShown))
		{
			_builtInItemsShown = std::set<std::string>(builtInItemsShown.begin(), builtInItemsShown.end());
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
	/// Tense music tracks for when aliens are in sight (the first one the game has is played).
	const std::vector<std::string> &getCombatMusic() const { return _combatMusic; }
	/// Does wearing this armor, or carrying this item, give night vision?
	bool givesNightVision(const std::string &type) const { return _nightVision.count(type) > 0; }
	/// Is a built-in item (fixed to its unit, or its special weapon) drawn in the hands all the same?
	bool showsBuiltInItem(const std::string &type) const { return _builtInItemsShown.count(type) > 0; }
	/// The weather of a terrain data set ("rain", "snow", "dust", ...), or empty for none.
	std::string getWeather(const std::string &dataSet) const
	{
		auto found = _weather.find(dataSet);
		return found != _weather.end() ? found->second : std::string();
	}
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
