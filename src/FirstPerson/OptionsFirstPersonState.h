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
 * OptionsFirstPersonState.h: the 3D VIEW tab of the options screen.
 */
#include <vector>
#include "../Engine/OptionInfo.h"
#include "../Menu/OptionsBaseState.h"

namespace OpenXcom
{

class TextList;

/**
 * "3D View" options tab, only offered when the OpenXcom3D mod is active:
 * first-person settings and key bindings (options in category STR_FIRST_PERSON).
 * Two sub-tabs share one list: Settings (the add-on's switches and numbers, plus the game's
 * frame rate limit and frame counter) and Controls (the add-on's keys, then the game's keys
 * that work in 3D). Left click raises or toggles a value, or picks a key to rebind; right
 * click lowers a value or clears a key.
 */
class OptionsFirstPersonState : public OptionsBaseState
{
	TextButton *_btnSettings, *_btnControlsList, *_owner; // The sub-tab buttons, and the one pressed now.
	TextList *_lstOptions;                              // The list of settings or keys.
	std::vector<OptionInfo> _settings, _keys;           // Copies of the options' infos (they still point at the real options).
	size_t _ownKeys = 0;                                // How many of _keys are the 3D view's own: they come first, then the game's keys that work in 3D (under a heading).
	int _selectedKey;                                   // List row of the key waiting for a new key (-1: none).
	OptionInfo *_selKey;                                // That key's option (nullptr: none).
	Uint8 _colorSel, _colorNormal;                      // Row colours: a key waiting for a new key, and normal.

	/// Gets the option shown on a list row (nullptr for the heading); isKey says if the keys are showing.
	OptionInfo *getEntry(int row, bool *isKey);
	/// Text for an option's current value (YES/NO, a number or its name, a key's name).
	std::string valueText(const OptionInfo &option) const;
	/// Name shown for an option.
	std::string label(const OptionInfo &option) const;
	/// Lists the settings or the key bindings, depending on the sub-tab.
	void updateList();
public:
	/// Creates the 3D View options tab.
	OptionsFirstPersonState(OptionsOrigin origin);
	/// Cleans up the tab.
	~OptionsFirstPersonState();
	/// Fills the list.
	void init() override;
	/// Changes a setting or picks a key to rebind.
	void lstOptionsClick(Action *action);
	/// Assigns the pressed key to the picked binding.
	void lstOptionsKeyPress(Action *action);
	/// Switches between the settings and the key bindings.
	void btnListPress(Action *action);
	/// Shows the description of the setting under the mouse.
	void lstOptionsMouseOver(Action *action);
	/// Clears the description.
	void lstOptionsMouseOut(Action *action);
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
