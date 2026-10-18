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
#include <vector>
#include "../Engine/OptionInfo.h"
#include "../Menu/OptionsBaseState.h"

namespace OpenXcom
{

class TextList;

/**
 * "3D View" options tab, only offered when the OpenXcom3D mod is active:
 * first-person settings and key bindings (options in category STR_FIRST_PERSON).
 */
class OptionsFirstPersonState : public OptionsBaseState
{
	TextButton *_btnSettings, *_btnControlsList, *_owner;
	TextList *_lstOptions;
	std::vector<OptionInfo> _settings, _keys;
	size_t _ownKeys = 0; // the 3D view's keys come first, then the game's keys that work in 3D (under a heading)
	int _selectedKey;
	OptionInfo *_selKey;
	Uint8 _colorSel, _colorNormal;

	OptionInfo *getEntry(int row, bool *isKey);
	std::string valueText(const OptionInfo &option) const;
	std::string label(const OptionInfo &option) const;
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
