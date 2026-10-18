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
#include "OptionsFirstPersonState.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string>
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Interface/FpsCounter.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/TextList.h"
#include "../Interface/Window.h"

namespace OpenXcom
{

namespace
{

/// Key names as SDL spells them, with each word capitalised (like the Controls tab).
std::string keyName(SDLKey key)
{
	if (key == SDLK_UNKNOWN)
	{
		return "";
	}
	std::string name = SDL_GetKeyName(key);
	bool start = true;
	for (char &c : name)
	{
		if (start) c = (char)toupper((unsigned char)c);
		start = (c == ' ');
	}
	return name;
}

/// The game's own keys that do something in 3D, listed (and rebindable) on our page too, with
/// what they do there. The enemy keys are added by number.
const char *const GameKeys[][2] = {
	{ "keyBattleLeft", "STR_FPS_KEY_TURN_LEFT" }, { "keyBattleRight", "STR_FPS_KEY_TURN_RIGHT" },
	{ "keyBattleUp", "STR_FPS_KEY_LOOK_UP" }, { "keyBattleDown", "STR_FPS_KEY_LOOK_DOWN" },
	{ "keyBattleKneel", "STR_FPS_KEY_KNEEL" }, { "keyBattleReload", "STR_FPS_KEY_RELOAD" },
	{ "keyBattleInventory", "STR_FPS_KEY_INVENTORY" }, { "keyBattleUseLeftHand", "STR_FPS_KEY_LEFT_HAND" },
	{ "keyBattleUseRightHand", "STR_FPS_KEY_RIGHT_HAND" }, { "keyBattlePersonalLighting", "STR_FPS_KEY_LIGHT" },
	{ "keyNightVisionToggle", "STR_FPS_KEY_NV_TOGGLE" }, { "keyBattleNextUnit", "STR_FPS_KEY_NEXT_UNIT" },
	{ "keyBattlePrevUnit", "STR_FPS_KEY_PREV_UNIT" }, { "keyBattleEndTurn", "STR_FPS_KEY_END_TURN" },
	{ "keyFps", "STR_FPS_KEY_COUNTER" }, { "keyQuickSave", "STR_FPS_KEY_QUICK_SAVE" }, { "keyQuickLoad", "STR_FPS_KEY_QUICK_LOAD" },
};
const char *const EnemyKeyPrefix = "keyBattleCenterEnemy";

/// Our name for one of the game's keys (empty for our own keys).
std::string gameKeyName(const std::string &id)
{
	for (const auto &key : GameKeys)
	{
		if (id == key[0]) return key[1];
	}
	return id.compare(0, strlen(EnemyKeyPrefix), EnemyKeyPrefix) == 0 ? "STR_FPS_KEY_ENEMY" : "";
}

}

/**
 * Creates the 3D View options tab.
 * @param origin Game section that opened the options.
 */
OptionsFirstPersonState::OptionsFirstPersonState(OptionsOrigin origin) : OptionsBaseState(origin),
	_selectedKey(-1), _selKey(nullptr)
{
	setCategory(_btnFirstPerson);

	// same layout as the Controls and Advanced tabs: a row of sub-tab buttons over a scrolling list
	_btnSettings = new TextButton(70, 16, 94, 8);
	_btnControlsList = new TextButton(70, 16, 168, 8);
	_lstOptions = new TextList(200, 120, 94, 26);
	_owner = _btnSettings;

	add(_btnSettings, "button", "controlsMenu");
	add(_btnControlsList, "button", "controlsMenu");
	if (origin != OPT_BATTLESCAPE)
	{
		add(_lstOptions, "optionLists", "controlsMenu");
	}
	else
	{
		add(_lstOptions, "optionLists", "battlescape");
	}
	centerAllSurfaces();

	_btnSettings->setText(tr("STR_FPS_SETTINGS"));
	_btnSettings->setGroup(&_owner);
	_btnSettings->onMousePress((ActionHandler)&OptionsFirstPersonState::btnListPress, SDL_BUTTON_LEFT);

	_btnControlsList->setText(tr("STR_FPS_CONTROLS"));
	_btnControlsList->setGroup(&_owner);
	_btnControlsList->onMousePress((ActionHandler)&OptionsFirstPersonState::btnListPress, SDL_BUTTON_LEFT);

	_lstOptions->setColumns(2, 152, 48);
	_lstOptions->setAlign(ALIGN_RIGHT, 1);
	_lstOptions->setWordWrap(true);
	_lstOptions->setSelectable(true);
	_lstOptions->setBackground(_window);
	_lstOptions->onMouseClick((ActionHandler)&OptionsFirstPersonState::lstOptionsClick, 0);
	_lstOptions->onKeyboardPress((ActionHandler)&OptionsFirstPersonState::lstOptionsKeyPress);
	_lstOptions->onMouseOver((ActionHandler)&OptionsFirstPersonState::lstOptionsMouseOver);
	_lstOptions->onMouseOut((ActionHandler)&OptionsFirstPersonState::lstOptionsMouseOut);
	_lstOptions->setFocus(true);

	_colorSel = _lstOptions->getScrollbarColor();
	_colorNormal = _lstOptions->getColor();

	const OptionInfo *frameCounter = nullptr, *frameLimit = nullptr;
	for (const auto &option : Options::getOptionInfo())
	{
		if (option.category() == "STR_FIRST_PERSON" && !option.description().empty())
		{
			(option.type() == OPTION_KEY ? _keys : _settings).push_back(option);
		}
		else if (option.id() == "fpsCounter")
		{
			frameCounter = &option;
		}
		else if (option.id() == "FPS" && option.type() == OPTION_INT)
		{
			frameLimit = &option;
		}
	}
	if (frameLimit)
	{
		_settings.push_back(*frameLimit); // the game's own frame rate limit, for fast screens
	}
	if (frameCounter)
	{
		_settings.push_back(*frameCounter); // the game's own frame counter (F7), handy in 3D
	}
	_ownKeys = _keys.size();
	std::vector<std::string> gameKeys;
	for (const auto &key : GameKeys) gameKeys.push_back(key[0]);
	for (int i = 1; i <= 10; ++i) gameKeys.push_back(EnemyKeyPrefix + std::to_string(i));
	for (const std::string &id : gameKeys)
	{
		for (const auto &option : Options::getOptionInfo())
		{
			if (option.id() == id && option.type() == OPTION_KEY) _keys.push_back(option);
		}
	}
}

OptionsFirstPersonState::~OptionsFirstPersonState()
{
}

void OptionsFirstPersonState::init()
{
	OptionsBaseState::init();
	updateList();
}

/**
 * Name shown for an option.
 */
std::string OptionsFirstPersonState::label(const OptionInfo &option) const
{
	if (option.id() == "fpsCounter")
	{
		return std::string(tr("STR_FPS_FRAME_COUNTER"));
	}
	if (option.id() == "FPS")
	{
		return std::string(tr("STR_FPS_FRAME_LIMIT"));
	}
	const std::string name = gameKeyName(option.id());
	if (name == "STR_FPS_KEY_ENEMY")
	{
		return std::string(tr(name).arg(std::stoi(option.id().substr(strlen(EnemyKeyPrefix)))));
	}
	return std::string(tr(name.empty() ? option.description() : name));
}

/**
 * Text for an option's current value.
 */
std::string OptionsFirstPersonState::valueText(const OptionInfo &option) const
{
	switch (option.type())
	{
	case OPTION_BOOL:
		return *option.asBool() ? std::string(tr("STR_YES")) : std::string(tr("STR_NO"));
	case OPTION_INT:
		if (option.id() == "fpsSpriteUpscale")
		{
			static const char *const names[] = { "STR_FPS_UPSCALE_OFF", "STR_FPS_UPSCALE_SMOOTH", "STR_FPS_UPSCALE_DETAIL" };
			return std::string(tr(names[std::clamp(*option.asInt(), 0, 2)]));
		}
		return option.id() == "FPS" && *option.asInt() == 0 ? std::string(tr("STR_FPS_UNLIMITED")) : std::to_string(*option.asInt());
	case OPTION_KEY:
		return keyName(*option.asKey());
	default:
		return "";
	}
}

/**
 * Lists the settings or the key bindings, depending on the sub-tab.
 */
void OptionsFirstPersonState::updateList()
{
	_lstOptions->clearList();
	_selectedKey = -1;
	_selKey = nullptr;
	const std::vector<OptionInfo> &list = _owner == _btnSettings ? _settings : _keys;
	for (size_t i = 0; i < list.size(); ++i)
	{
		const OptionInfo &option = list[i];
		if (&list == &_keys && i == _ownKeys)
		{
			_lstOptions->addRow(1, tr("STR_FPS_GAME_KEYS").c_str());
			_lstOptions->setRowColor(_lstOptions->getLastRowIndex(), _lstOptions->getSecondaryColor());
		}
		_lstOptions->addRow(2, label(option).c_str(), valueText(option).c_str());
		if (option.id() == "fpsSoldierPreview")
		{
			_lstOptions->setRowColor(_lstOptions->getLastRowIndex(), _lstOptions->getSecondaryColor()); // the headline feature stands out
		}
	}
}

/**
 * Gets the option shown on a list row, if any.
 */
OptionInfo *OptionsFirstPersonState::getEntry(int row, bool *isKey)
{
	std::vector<OptionInfo> &list = (_owner == _btnSettings) ? _settings : _keys;
	*isKey = (_owner != _btnSettings);
	if (*isKey && _ownKeys < list.size() && row >= (int)_ownKeys)
	{
		if (row == (int)_ownKeys) return nullptr; // the heading of the game's keys
		--row;
	}
	if (row < 0 || row >= (int)list.size())
	{
		return nullptr;
	}
	return &list[row];
}

/**
 * Switches between the settings and the key bindings.
 */
void OptionsFirstPersonState::btnListPress(Action *)
{
	updateList();
}

/**
 * Left click raises a value / toggles / picks a key to rebind; right click lowers a value / clears a key.
 */
void OptionsFirstPersonState::lstOptionsClick(Action *action)
{
	const Uint8 button = action->getDetails()->button.button;
	if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
	{
		return;
	}
	if (_selectedKey != -1)
	{
		_lstOptions->setCellColor(_selectedKey, 0, _colorNormal);
		_lstOptions->setCellColor(_selectedKey, 1, _colorNormal);
		_selectedKey = -1;
		_selKey = nullptr;
	}
	const int row = _lstOptions->getSelectedRow();
	bool isKey = false;
	OptionInfo *option = getEntry(row, &isKey);
	if (!option)
	{
		return;
	}
	if (isKey)
	{
		if (button == SDL_BUTTON_LEFT)
		{
			_selectedKey = row;
			_selKey = option;
			_lstOptions->setCellColor(row, 0, _colorSel);
			_lstOptions->setCellColor(row, 1, _colorSel);
		}
		else
		{
			*option->asKey() = SDLK_UNKNOWN;
			_lstOptions->setCellText(row, 1, "");
		}
		return;
	}
	if (option->type() == OPTION_BOOL)
	{
		*option->asBool() = !*option->asBool();
		if (option->asBool() == &Options::fpsCounter)
		{
			_game->getFpsCounter()->setVisible(Options::fpsCounter);
		}
	}
	else if (option->type() == OPTION_INT)
	{
		int *value = option->asInt();
		const int direction = (button == SDL_BUTTON_LEFT) ? 1 : -1;
		if (value == &Options::fpsFieldOfView)
		{
			*value = std::clamp(*value + 5 * direction, 50, 110);
		}
		else if (value == &Options::fpsMouseSensitivity)
		{
			*value = std::clamp(*value + direction, 1, 20);
		}
		else if (value == &Options::fpsWalkSpeed)
		{
			*value = std::clamp(*value + direction, 1, 8);
		}
		else if (value == &Options::fpsParticles)
		{
			*value = (*value + direction + 3) % 3;
		}
		else if (value == &Options::fpsSpriteUpscale)
		{
			*value = (*value + direction + 3) % 3;
		}
		else if (value == &Options::fpsEyeLower)
		{
			*value = std::clamp(*value + direction, 0, 6);
		}
		else if (value == &Options::fpsPixelSize)
		{
			*value = std::clamp(*value + direction, 1, 3);
		}
		else if (value == &Options::fpsFlightCost)
		{
			*value = std::clamp(*value + direction, 0, 24);
		}
		else if (value == &Options::FPS)
		{
			// the game's frame rate limit, in the steps screens have (0 = unlimited)
			static const int steps[] = { 60, 75, 90, 120, 144, 165, 240, 0 };
			const int count = (int)(sizeof(steps) / sizeof(steps[0]));
			int at = 0;
			while (at < count - 1 && steps[at] != *value) ++at;
			*value = steps[std::clamp(at + direction, 0, count - 1)];
		}
	}
	_lstOptions->setCellText(row, 1, valueText(*option));
}

/**
 * Assigns the pressed key to the picked binding (modifier keys can't be bound, as in the Controls tab).
 */
void OptionsFirstPersonState::lstOptionsKeyPress(Action *action)
{
	if (_selectedKey == -1)
	{
		return;
	}
	const SDLKey key = action->getDetails()->key.keysym.sym;
	if (key != 0 && key != SDLK_LSHIFT && key != SDLK_LALT && key != SDLK_LCTRL &&
		key != SDLK_RSHIFT && key != SDLK_RALT && key != SDLK_RCTRL)
	{
		*_selKey->asKey() = key;
		_lstOptions->setCellText(_selectedKey, 1, keyName(key));
	}
	_lstOptions->setCellColor(_selectedKey, 0, _colorNormal);
	_lstOptions->setCellColor(_selectedKey, 1, _colorNormal);
	_selectedKey = -1;
	_selKey = nullptr;
}

void OptionsFirstPersonState::lstOptionsMouseOver(Action *)
{
	bool isKey = false;
	const OptionInfo *option = getEntry(_lstOptions->getSelectedRow(), &isKey);
	std::string desc;
	if (option)
	{
		const std::string name = gameKeyName(option->id());
		desc = option->id() == "fpsCounter" ? std::string(tr("STR_FPS_FRAME_COUNTER_DESC"))
			: option->id() == "FPS" ? std::string(tr("STR_FPS_FRAME_LIMIT_DESC"))
			: std::string(tr((name.empty() ? option->description() : name) + "_DESC"));
	}
	_txtTooltip->setText(desc);
}

void OptionsFirstPersonState::lstOptionsMouseOut(Action *)
{
	_txtTooltip->setText("");
}

}
