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
#include <cctype>
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
 * terrain has, which battles have another world's sky, which music plays in combat, which armors
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
	/// Whether a weapon is held as a bow (upright, its curve to the target): worked out from its name (AUTO), or said (NO, YES).
	enum HeldAsBow { BOW_AUTO = -1, BOW_NO, BOW_YES };
	/// What a weapon's shots are: worked out (AUTO), a gun's rounds (POWDER) or objects flying (OBJECT: arrows, bolts, harpoons).
	enum ShotKind { SHOT_AUTO = -1, SHOT_POWDER, SHOT_OBJECT };
	/// One item's entry in the item table (`items:`): what the 3D view isn't left to work out.
	struct ItemLook
	{
		HeldShape held = HELD_AUTO; // how its picture is given volume
		ShotKind shot = SHOT_AUTO;  // what its shots are (a weapon's, or an ammo's)
		HeldAsBow bow = BOW_AUTO;   // held as a bow
		int underside = 0;          // the side of its picture that is its underside in the hand: 1 left, 2 right, 3 top, 4 bottom (0: measured)
	};
private:
public:
	/// What the unit table (`units:`) says of a unit, by its names: whether its voxel body is made as a long one on
	/// four legs (1), or as ever (0; -1: nothing said, the same); and how much of the body shows, in hundredths (100
	/// all of it; less: see-through, a ghost), or -1 for nothing said. A unit the table doesn't name is made by the
	/// general rules alone: what is done for one kind of unit touches no other.
	/// And for a named unit's body: torsoDepth, the most its torso's slices are deep below the head, in hundredths of their
	/// width, cut at the back (0: no limit); backTop, whether what faces up is painted by the back drawing (1: the top of a head is hair) or as ever,
	/// along the front or back drawing's slant (0); -1 for nothing said.
	struct UnitLook
	{
		int beast = -1;
		int shows = -1;
		int torsoDepth = -1;
		int backTop = -1;
		int frontal = -1; // (the body kept to its front and back drawings' outlines: 1; a big walker cut by its four drawings from front, back and sides: 2; as ever: 0)
		int animated = -1; // (a body for each frame of the sprite's own animation, if it has one: 1 or nothing said; one body: 0; one body that rises, sinks and swells in a slow beat: 2)
		int relief = -1; // (the body is its one drawing given volume: 1; carved from its drawings: 0; nothing said: by what its drawings are)
		int arms = -1; // (a bit each: 1 its arms outside the body, 2 coloured along their length, 4 painted by their own drawing alone, 8 posed on their bones, 16 the elbow of an arm that holds its item up where its drawing ends, 32 an arm that holds an item as long as its own drawing is; 0 as drawn)
		int standard = -1; // (a person: its body made the standard humanoid: 1; as its drawings alone leave it, as ever: 0)
		int roundTrunk = -1; // (a long body's trunk as broad as it is tall: 1; as broad as its frontal drawings have it, as ever: 0)
		int faceHead = -1; // (its head painted as a face on hair or a helmet: 1; by its front and back drawings as the rest of it, as ever: 0)
		int tail = -1; // (its legs' frames draw a tail too: 1; every run of theirs is a leg's, as ever: 0)
		int legs = -1; // (thin legs spread round its body, made anew: how many, all told; 0 none, as ever)
		int barrels = -1; // (a turret's or a tank's gun: how many barrels it is made with; 0 none, the turret as its drawings leave it)
		int hullPaint = -1; // (drawn on one frame, painted as a hull is, each cube by the drawing that looks onto it most squarely: 1; by its front and back drawings, as ever: 0)
		int smoothBack = -1; // (no hollow across its back under a head of hair or a crest: 1; its back as its drawings leave it, as ever: 0)
		int sparks = -1; // (big and flying, it hovers on a rain of sparks: 1; nothing under it, as every other unit: 0)
		int kneelLow = -1; // (kneeling, its body sat down on its heel, as low as the game's rules have a kneeling unit: 1; as its kneeling drawings have it, as ever: 0)
		int flatTop = -1, flatUnder = -1; // (how much of what stands above its widest level, and of what hangs under it, the body keeps, in hundredths; 100 as ever)
		int smallTurret = -1; // (one tile big and unable to move: made a plain block with its gun, as the big turrets are: 1; carved from its drawings, as ever: 0)
		int onFloor = -1; // (standing, its body's lowest cube is on the floor: 1; as high over it as its drawings leave it, as ever: 0)
		int waistDrawn = -1; // (the rows where its trunk meets its legs are as its drawings have them: 1; filled out to the trunk's fullest row near its foot, as ever: 0)
	};
	/// Does a name hold a word of the unit table? A name's words are its runs of letters (STR_GHOST_GAL: STR, GHOST,
	/// GAL; BLACK_CAT.PCK: BLACK, CAT, PCK), and a pattern is a word (CAT: not CATGIRL), a word's start (GHOST*:
	/// GHOSTLY), a word's end (*DOGE: BLOODOGE) or any part of a word (*SPIDER*); or the whole name itself (one unit:
	/// BAN_6.PCK, STR_MY_UNIT_ARMOR). Capitals or not is the same. A star alone is every unit.
	static bool nameHas(const std::string &name, const std::string &pattern)
	{
		if (pattern == "*") return !name.empty();
		if (name.size() == pattern.size() && std::equal(name.begin(), name.end(), pattern.begin(),
			[](char one, char other) { return std::toupper((unsigned char)one) == std::toupper((unsigned char)other); })) return true;
		const bool openStart = !pattern.empty() && pattern.front() == '*', openEnd = pattern.size() > 1 && pattern.back() == '*';
		std::string wanted = pattern.substr(openStart ? 1 : 0, pattern.size() - (openStart ? 1 : 0) - (openEnd ? 1 : 0));
		for (char &letter : wanted) letter = (char)std::toupper((unsigned char)letter);
		if (wanted.empty()) return false;
		for (size_t at = 0; at < name.size();)
		{
			if (!std::isalpha((unsigned char)name[at])) { ++at; continue; }
			size_t end = at;
			std::string word;
			while (end < name.size() && std::isalpha((unsigned char)name[end])) word += (char)std::toupper((unsigned char)name[end++]);
			at = end;
			if (word.size() < wanted.size()) continue;
			if (openStart && openEnd ? word.find(wanted) != std::string::npos
				: openStart ? word.compare(word.size() - wanted.size(), wanted.size(), wanted) == 0
				: openEnd ? word.compare(0, wanted.size(), wanted) == 0
				: word == wanted) return true;
		}
		return false;
	}
private:
	bool _enabled;
	int _maxViewDistance;                        // in tiles
	std::vector<std::pair<std::vector<std::string>, UnitLook>> _unitLooks; // the unit table: each entry's name patterns and what it sets
	std::map<std::string, Windows> _windows;     // terrain data set -> its windows
	std::map<std::pair<std::string, int>, std::vector<Shape>> _shapes; // (terrain data set, record) -> its shapes, one per game
	std::map<std::string, std::string> _weather; // terrain data set -> rain, snow, dust, ...
	std::map<std::string, std::string> _enviroWeather; // battle environment (enviroEffects type) -> rain, snow, dust, ... (before the data sets')
	std::map<std::string, std::string> _enviroSkies, _missionSkies, _terrainSkies; // environment, mission type, terrain -> its sky (space, moon, ...)
	std::set<std::string> _water;                // terrain data sets whose floors are water (the sea)
	std::map<std::pair<std::string, int>, int> _turned; // (terrain data set, record) -> the height from which its frames are turned from their art (see getTurnedFrom)
	std::map<std::string, std::set<int>> _waterRecords; // terrain data set -> its records that are water (the sea round a ship, among its parts)
	struct StillWater { Uint16 groups = 0; std::set<int> records; }; // the palette's colour groups (a bit each) that are water on a data set's floors; its records, or all
	std::map<std::string, StillWater> _stillWater;  // terrain data set -> its still water (a pond, a swamp, a paddy)
	std::vector<std::string> _combatMusic;       // tense tracks to try, first one the game has wins
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

		// Combat music, and the built-in items drawn in the hands (a
		// list replaces the earlier one).
		reader.tryRead("combatMusic", _combatMusic);
		std::vector<std::string> builtInItemsShown;
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
		// Still water (a pond, a swamp, a paddy, a puddle, the shallows of a beach): terrain data sets on whose floors
		// the pixels of some of the palette's colour groups are water, which mirrors (setting "Reflections") and keeps
		// its own colours; each `{ dataSet, groups: [0..15], records: [...] }`, the records left out for all of its
		// floors. Unlike the sea's (`water:` below), such a floor is as its art has it, bank and reeds and all, and
		// nothing of it goes on beyond the map's edge. A list replaces the earlier one.
		if (const auto& still = reader["stillWater"])
		{
			_stillWater.clear();
			for (const auto& entry : still.children())
			{
				std::string dataSet;
				std::vector<int> groups, records;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("groups", groups);
				entry.tryRead("records", records);
				StillWater &water = _stillWater[dataSet];
				for (int group : groups) if (group >= 0 && group < 16) water.groups |= (Uint16)(1 << group);
				water.records.insert(records.begin(), records.end());
			}
		}

		// The terrain data sets whose floors are water: where it reaches the map's edge, the sea
		// goes on beyond it (FirstPersonView::chooseGround). A data set's name for all of its
		// floors, or its name and records for some (a ship's data set with the sea round the ship
		// among its parts). A list replaces the earlier one.
		if (const auto& water = reader["water"])
		{
			_water.clear();
			_waterRecords.clear();
			for (const auto& entry : water.children())
			{
				if (entry.isMap())
				{
					std::string dataSet;
					std::vector<int> records;
					entry.tryRead("dataSet", dataSet);
					entry.tryRead("records", records);
					_waterRecords[dataSet].insert(records.begin(), records.end());
				}
				else
				{
					_water.insert(entry.readVal<std::string>());
				}
			}
		}

		// Tile parts that are a thing in motion round its own upright axis (a pedestal whose collar and piston rise and
		// sink): from a height up, each frame's shape is turned from that frame's art, as on a lathe, where the game
		// has one shape for all the frames (FirstPersonView::turnedFrom). Named one by one: { dataSet, records, from }.
		// A list replaces the earlier one.
		if (const auto& turned = reader["turned"])
		{
			_turned.clear();
			for (const auto& entry : turned.children())
			{
				std::string dataSet;
				std::vector<int> records;
				int from = 0;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("records", records);
				entry.tryRead("from", from);
				for (const int record : records) _turned[std::make_pair(dataSet, record)] = from;
			}
		}

		// The item table: groups of item types (as the mods name them), each with what it sets:
		// held (cushion, plate, or auto), shot (powder, object, or auto) and bow (true or false: held as a bow or not). A later entry
		// overrides what it sets for an item, and leaves the rest.
		if (const auto& items = reader["items"])
		{
			for (const auto& entry : items.children())
			{
				std::vector<std::string> types;
				std::string held, shot, underside;
				bool bow = false;
				const bool bowSaid = entry.tryRead("bow", bow);
				entry.tryRead("types", types);
				entry.tryRead("held", held);
				entry.tryRead("shot", shot);
				entry.tryRead("underside", underside);
				for (const std::string &type : types)
				{
					ItemLook &look = _itemLooks[type];
					if (!held.empty()) look.held = held == "plate" ? HELD_PLATE : held == "cushion" ? HELD_CUSHION : HELD_AUTO;
					if (!shot.empty()) look.shot = shot == "object" ? SHOT_OBJECT : shot == "powder" ? SHOT_POWDER : SHOT_AUTO;
					if (bowSaid) look.bow = bow ? BOW_YES : BOW_NO;
					if (!underside.empty()) look.underside = underside == "left" ? 1 : underside == "right" ? 2 : underside == "top" ? 3 : underside == "bottom" ? 4 : 0;
				}
			}
		}

		// The unit table: groups of name patterns (see nameHas), each with what it says of the units whose names (the
		// unit's type, its armor's, the armor's sprite sheet, its race) hold one: shape (beast: a long body on four legs;
		// relief: its one drawing given volume, see VoxelBodies::Routine; turret: one tile big and unable to move, made a plain
		// block with its gun as the big turrets are; upright: carved from its drawings, as ever) and seeThrough (how much of the voxel body is left out, in hundredths: 50 for a ghost). A
		// later entry overrides what it sets, and leaves the rest. What the voxel bodies do for one kind of unit they
		// do for the units named here and no others (the owner: "The future voxelisation rules will be per unit ... if
		// we dont have unit name in table we will apply some general rules").
		if (const auto& units = reader["units"])
		{
			for (const auto& entry : units.children())
			{
				std::vector<std::string> names;
				std::string shape, top, sculpt, animation, arms, humanoid, trunk, head, tail, paint, kneel, hover, back, stands, waist;
				int seeThrough = 0, torsoDepth = 0, flat = 100, flatUnder = 100, legs = 0, barrels = 0;
				UnitLook look;
				entry.tryRead("names", names);
				if (entry.tryRead("shape", shape))
				{
					look.beast = shape == "beast" ? 1 : 0;
					look.relief = shape == "relief" ? 1 : 0;
					look.smallTurret = shape == "turret" ? 1 : 0;
				}
				if (entry.tryRead("seeThrough", seeThrough)) look.shows = 100 - std::clamp(seeThrough, 0, 90);
				if (entry.tryRead("torsoDepth", torsoDepth)) look.torsoDepth = std::clamp(torsoDepth, 0, 200);
				if (entry.tryRead("top", top)) look.backTop = top == "back" ? 1 : 0;
				if (entry.tryRead("sculpt", sculpt)) look.frontal = sculpt == "frontal" ? 1 : sculpt == "sides" ? 2 : 0;
				if (entry.tryRead("animation", animation)) look.animated = animation == "sprite" ? 1 : animation == "pulse" ? 2 : 0;
				if (entry.tryRead("arms", arms))
				{
					// (one word or several: "own posed")
					look.arms = 0;
					for (size_t at = 0; at < arms.size();)
					{
						const size_t end = std::min(arms.find_first_of(" ,", at), arms.size());
						const std::string word = arms.substr(at, end - at);
						look.arms |= word == "outside" ? 1 : word == "along" ? 2 : word == "both" ? 3 : word == "own" ? 4 : word == "posed" ? 8 : word == "elbow" ? 16 : word == "reach" ? 32 : 0;
						at = end + 1;
					}
				}
				if (entry.tryRead("trunk", trunk)) look.roundTrunk = trunk == "round" ? 1 : 0;
				if (entry.tryRead("head", head)) look.faceHead = head == "face" ? 1 : 0;
				if (entry.tryRead("tail", tail)) look.tail = tail == "drawn" ? 1 : 0;
				if (entry.tryRead("paint", paint)) look.hullPaint = paint == "hull" ? 1 : 0;
				if (entry.tryRead("kneel", kneel)) look.kneelLow = kneel == "low" ? 1 : 0;
				if (entry.tryRead("hover", hover)) look.sparks = hover == "sparks" ? 1 : 0;
				if (entry.tryRead("back", back)) look.smoothBack = back == "smooth" ? 1 : 0;
				if (entry.tryRead("stands", stands)) look.onFloor = stands == "floor" ? 1 : 0;
				if (entry.tryRead("waist", waist)) look.waistDrawn = waist == "drawn" ? 1 : 0;
				if (entry.tryRead("legs", legs)) look.legs = std::clamp(legs, 0, 14);
				if (entry.tryRead("barrels", barrels)) look.barrels = std::clamp(barrels, 0, 4);
				if (entry.tryRead("humanoid", humanoid)) look.standard = humanoid == "standard" ? 1 : 0;
				if (entry.tryRead("flat", flat)) look.flatTop = std::clamp(flat, 10, 100);
				if (entry.tryRead("flatUnder", flatUnder)) look.flatUnder = std::clamp(flatUnder, 10, 100);
				if (!names.empty()) _unitLooks.push_back({ names, look });
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

		// Weather: per terrain data set, or per battle environment (its enviroEffects, as the big
		// mods name them: acid rain, a dust storm), its kind ("rain", "snow", "dust", ...).
		if (const auto& weather = reader["weather"])
		{
			for (const auto& entry : weather.children())
			{
				std::string dataSet, type;
				std::vector<std::string> enviroEffects;
				entry.tryRead("dataSet", dataSet);
				entry.tryRead("enviroEffects", enviroEffects);
				entry.tryRead("type", type);
				if (!dataSet.empty()) _weather[dataSet] = type;
				for (const std::string &enviro : enviroEffects) _enviroWeather[enviro] = type;
			}
		}

		// Skies that aren't the Earth's open air: per mission (alien deployment) type, map terrain and
		// battle environment (its enviroEffects), the sky ("space", "orbit", "moon", "mars", "sea",
		// "void", or "earth" for the Earth's own). A later entry (or a later mod's) overrides.
		if (const auto& skies = reader["skies"])
		{
			for (const auto& entry : skies.children())
			{
				std::string sky;
				std::vector<std::string> enviroEffects, missions, terrains;
				entry.tryRead("sky", sky);
				entry.tryRead("enviroEffects", enviroEffects);
				entry.tryRead("missions", missions);
				entry.tryRead("terrains", terrains);
				for (const std::string &enviro : enviroEffects) _enviroSkies[enviro] = sky;
				for (const std::string &mission : missions) _missionSkies[mission] = sky;
				for (const std::string &terrain : terrains) _terrainSkies[terrain] = sky;
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
	/// Is a mission (its alien deployment type) listed as underground all the way up?
	bool isUnderground(const std::string &mission) const { return _undergroundMissions.count(mission) > 0; }
	/// Is a built-in item (fixed to its unit, or its special weapon) drawn in the hands all the same?
	bool showsBuiltInItem(const std::string &type) const { return _builtInItemsShown.count(type) > 0; }
	/// What the unit table says of a unit with these names (its type, its armor's, the sprite sheet, its race): each
	/// thing as the last entry that sets it and matches one of the names has it.
	UnitLook getUnitLook(const std::vector<std::string> &names) const
	{
		UnitLook look;
		for (const auto &entry : _unitLooks)
		{
			bool matches = false;
			for (const std::string &pattern : entry.first)
				for (const std::string &name : names) matches = matches || nameHas(name, pattern);
			if (!matches) continue;
			if (entry.second.beast != -1) look.beast = entry.second.beast;
			if (entry.second.shows != -1) look.shows = entry.second.shows;
			if (entry.second.torsoDepth != -1) look.torsoDepth = entry.second.torsoDepth;
			if (entry.second.backTop != -1) look.backTop = entry.second.backTop;
			if (entry.second.frontal != -1) look.frontal = entry.second.frontal;
			if (entry.second.animated != -1) look.animated = entry.second.animated;
			if (entry.second.relief != -1) look.relief = entry.second.relief;
			if (entry.second.arms != -1) look.arms = entry.second.arms;
			if (entry.second.standard != -1) look.standard = entry.second.standard;
			if (entry.second.roundTrunk != -1) look.roundTrunk = entry.second.roundTrunk;
			if (entry.second.faceHead != -1) look.faceHead = entry.second.faceHead;
			if (entry.second.tail != -1) look.tail = entry.second.tail;
			if (entry.second.hullPaint != -1) look.hullPaint = entry.second.hullPaint;
			if (entry.second.kneelLow != -1) look.kneelLow = entry.second.kneelLow;
			if (entry.second.sparks != -1) look.sparks = entry.second.sparks;
			if (entry.second.smoothBack != -1) look.smoothBack = entry.second.smoothBack;
			if (entry.second.legs != -1) look.legs = entry.second.legs;
			if (entry.second.barrels != -1) look.barrels = entry.second.barrels;
			if (entry.second.flatTop != -1) look.flatTop = entry.second.flatTop;
			if (entry.second.flatUnder != -1) look.flatUnder = entry.second.flatUnder;
			if (entry.second.smallTurret != -1) look.smallTurret = entry.second.smallTurret;
			if (entry.second.onFloor != -1) look.onFloor = entry.second.onFloor;
			if (entry.second.waistDrawn != -1) look.waistDrawn = entry.second.waistDrawn;
		}
		return look;
	}
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
	/// The height from which a terrain record's frames are turned from their art (the table `turned:`), or -1 if it isn't named.
	int getTurnedFrom(const std::string &dataSet, int record) const
	{
		auto found = _turned.find(std::make_pair(dataSet, record));
		return found != _turned.end() ? found->second : -1;
	}
	/// The palette's colour groups (a bit each) that are still water on a terrain record's floor (`stillWater:`), or 0.
	Uint16 getStillWater(const std::string &dataSet, int record) const
	{
		auto found = _stillWater.find(dataSet);
		return found != _stillWater.end() && (found->second.records.empty() || found->second.records.count(record) > 0) ? found->second.groups : (Uint16)0;
	}
	/// Is a terrain record's floor water (the sea round islands, ports and ships)?
	bool isWater(const std::string &dataSet, int record) const
	{
		auto found = _waterRecords.find(dataSet);
		return _water.count(dataSet) > 0 || (found != _waterRecords.end() && found->second.count(record) > 0);
	}
	/// The weather of a terrain data set ("rain", "snow", "dust", ...), or empty for none.
	std::string getWeather(const std::string &dataSet) const
	{
		auto found = _weather.find(dataSet);
		return found != _weather.end() ? found->second : std::string();
	}
	/// The weather of a battle environment (an enviroEffects type), or empty for none (then its terrain data sets tell).
	std::string getEnviroWeather(const std::string &enviro) const
	{
		auto found = _enviroWeather.find(enviro);
		return found != _enviroWeather.end() ? found->second : std::string();
	}
	/// A battle's sky, the most particular first: its mission type's, else that of the first of its map's terrains that
	/// has one, else its environment's (an enviroEffects type, which many missions share); empty for the Earth's.
	std::string getSky(const std::string &enviro, const std::string &mission, const std::vector<std::string> &terrains) const
	{
		auto found = _missionSkies.find(mission);
		if (found != _missionSkies.end()) return found->second;
		for (const std::string &terrain : terrains)
		{
			found = _terrainSkies.find(terrain);
			if (found != _terrainSkies.end()) return found->second;
		}
		found = _enviroSkies.find(enviro);
		return found != _enviroSkies.end() ? found->second : std::string();
	}
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
