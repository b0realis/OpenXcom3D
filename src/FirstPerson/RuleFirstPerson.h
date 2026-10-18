#pragma once
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
 * The `firstPerson:` ruleset section. The first-person mode is engine code, but it
 * only switches on when an active mod (OpenXcom3D) declares `firstPerson: enabled: true`.
 * The mod also tells it which terrain records are windows (and which of their colours
 * are glass), and what weather each terrain has.
 */
class RuleFirstPerson
{
public:
	/// Window records of one terrain data set, and the sprite colours that are glass.
	struct Windows
	{
		std::set<int> records;
		std::bitset<256> glass;
	};
private:
	bool _enabled;
	int _maxViewDistance;
	std::map<std::string, Windows> _windows;
	std::map<std::string, std::string> _weather; // terrain data set -> rain, snow, dust, ...
	std::vector<std::string> _combatMusic;        // tense tracks to try, first one the game has wins
	std::set<std::string> _nightVision;           // armors and items that give their wearer night vision
public:
	/// Creates the rule with the feature switched off.
	RuleFirstPerson() : _enabled(false), _maxViewDistance(60), _combatMusic({ "GMDEFEND", "GMDEFBAS" }) { }
	/// Loads the `firstPerson:` node; later mods override earlier ones.
	void load(const YAML::YamlNodeReader& reader)
	{
		reader.tryRead("enabled", _enabled);
		reader.tryRead("maxViewDistance", _maxViewDistance);
		_maxViewDistance = std::clamp(_maxViewDistance, 10, 200);
		if (const auto& windows = reader["windows"])
		{
			for (const auto& entry : windows.children())
			{
				std::string dataSet;
				std::vector<int> records, colors;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("records", records);
				entry.tryRead("glassColors", colors);
				Windows &w = _windows[dataSet];
				w.records.insert(records.begin(), records.end());
				for (int c : colors)
				{
					if (c > 0 && c < 256) w.glass.set(c);
				}
			}
		}
		reader.tryRead("combatMusic", _combatMusic);
		std::vector<std::string> nightVision;
		if (reader.tryRead("nightVision", nightVision))
		{
			_nightVision = std::set<std::string>(nightVision.begin(), nightVision.end());
		}
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
	/// Window records of a terrain data set, or nullptr.
	const Windows *getWindows(const std::string &dataSet) const
	{
		auto it = _windows.find(dataSet);
		return it != _windows.end() ? &it->second : nullptr;
	}
	/// Tense music tracks for when aliens are in sight (the first one the game has is played).
	const std::vector<std::string> &getCombatMusic() const { return _combatMusic; }
	/// Does wearing this armor, or carrying this item, give night vision?
	bool givesNightVision(const std::string &type) const { return _nightVision.count(type) > 0; }
	/// Weather of a terrain data set ("rain", "snow", "dust", ...), or empty.
	std::string getWeather(const std::string &dataSet) const
	{
		auto it = _weather.find(dataSet);
		return it != _weather.end() ? it->second : std::string();
	}
};

}
