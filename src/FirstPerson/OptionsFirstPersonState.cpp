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
 * OptionsFirstPersonState.cpp: the 3D VIEW tab of the options screen, where every setting of
 * the first-person mode is switched and its keys are set.
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

/**
 * Gives a key's name as SDL spells it, with each word capitalised (as the Controls tab shows
 * them), for example "Page Up".
 * @param key The key.
 * @return Its name, or an empty text for no key (an unbound action).
 */
std::string keyName(SDLKey key)
{
	if (key == SDLK_UNKNOWN)
	{
		return "";
	}
	// Capitalise the first letter of every word: the first character, and each one after a space.
	std::string name = SDL_GetKeyName(key);
	bool wordStart = true;
	for (char &character : name)
	{
		if (wordStart) character = (char)toupper((unsigned char)character);
		wordStart = (character == ' ');
	}
	return name;
}

/// The game's own keys that do something in 3D, listed (and rebindable) on our page too, with
/// what they do there: each entry is the option's id and our label for it. The keys that
/// centre on an enemy (keyBattleCenterEnemy1 to 10) are added by number.
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
/// The start of the ids of the game's "centre on enemy" keys; the enemy's number follows (1 to 10).
const char *const EnemyKeyPrefix = "keyBattleCenterEnemy";

/**
 * Gives our label for one of the game's keys listed on our page, saying what it does in 3D.
 * @param optionId The option's id, like "keyBattleKneel".
 * @return The label's translation key ("STR_FPS_KEY_ENEMY" for every enemy key, whose number
 *  is filled in later), or an empty text for our own keys, which use their own description.
 */
std::string gameKeyName(const std::string &optionId)
{
	// One of the listed game keys?
	for (const auto &gameKey : GameKeys)
	{
		if (optionId == gameKey[0]) return gameKey[1];
	}
	// Otherwise one of the enemy keys, or not a game key at all.
	return optionId.compare(0, strlen(EnemyKeyPrefix), EnemyKeyPrefix) == 0 ? "STR_FPS_KEY_ENEMY" : "";
}

}

/**
 * Creates the 3D View options tab: two sub-tabs (the settings and the key bindings) over one
 * list. It gathers the add-on's options (category STR_FIRST_PERSON), two of the game's own
 * settings that matter in 3D (the frame rate limit and the frame counter), and the game's keys
 * that work in 3D.
 * @param origin Game section that opened the options.
 */
OptionsFirstPersonState::OptionsFirstPersonState(OptionsOrigin origin) : OptionsBaseState(origin),
	_selectedKey(-1), _selKey(nullptr)
{
	// Show this tab's button pressed in the column of tabs.
	setCategory(_btnFirstPerson);

	// Same layout as the Controls and Advanced tabs: a row of sub-tab buttons over a scrolling
	// list (sizes and positions in pixels of the game's 320x200 screen, all centred on a bigger
	// one below). The Settings sub-tab is the one pressed at first.
	_btnSettings = new TextButton(70, 16, 94, 8);
	_btnControlsList = new TextButton(70, 16, 168, 8);
	_lstOptions = new TextList(200, 120, 94, 26);
	_owner = _btnSettings;

	// Colours from the interface rules: the Controls menu's, or the battlescape's when the
	// options were opened in a battle.
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

	// The two sub-tab buttons work as a group (one pressed at a time); pressing either one
	// refills the list.
	_btnSettings->setText(tr("STR_FPS_SETTINGS"));
	_btnSettings->setGroup(&_owner);
	_btnSettings->onMousePress((ActionHandler)&OptionsFirstPersonState::btnListPress, SDL_BUTTON_LEFT);

	_btnControlsList->setText(tr("STR_FPS_CONTROLS"));
	_btnControlsList->setGroup(&_owner);
	_btnControlsList->onMousePress((ActionHandler)&OptionsFirstPersonState::btnListPress, SDL_BUTTON_LEFT);

	// The list: names on the left (152 pixels wide), values right-aligned on the right (48
	// pixels). Clicks change values, key presses rebind keys, and hovering a row shows its
	// description in the tooltip line.
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

	// A key binding waiting for its new key is shown in the scrollbar's colour.
	_colorSel = _lstOptions->getScrollbarColor();
	_colorNormal = _lstOptions->getColor();

	// Gather the add-on's options with a label (the switches and numbers as settings, the rest
	// as keys), and find two of the game's own settings on the way.
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
	// The game's two settings go at the end of the settings.
	if (frameLimit)
	{
		_settings.push_back(*frameLimit); // The game's own frame rate limit, for fast screens.
	}
	if (frameCounter)
	{
		_settings.push_back(*frameCounter); // The game's own frame counter (F7), handy in 3D.
	}
	// After our own keys come the game's keys that work in 3D, in the order of GameKeys and
	// then the ten enemy keys.
	_ownKeys = _keys.size();
	std::vector<std::string> gameKeyIds;
	for (const auto &gameKey : GameKeys) gameKeyIds.push_back(gameKey[0]);
	for (int enemyNumber = 1; enemyNumber <= 10; ++enemyNumber) gameKeyIds.push_back(EnemyKeyPrefix + std::to_string(enemyNumber));
	for (const std::string &keyId : gameKeyIds)
	{
		for (const auto &option : Options::getOptionInfo())
		{
			if (option.id() == keyId && option.type() == OPTION_KEY) _keys.push_back(option);
		}
	}
}

/**
 * Cleans up the tab. There is nothing of its own to free: the state deletes the surfaces it
 * was given.
 */
OptionsFirstPersonState::~OptionsFirstPersonState()
{
}

/**
 * Fills the list each time the tab comes up, after the base state's own setup.
 */
void OptionsFirstPersonState::init()
{
	OptionsBaseState::init();
	updateList();
}

/**
 * Gives the name shown for an option: our labels for the game's two settings and its keys,
 * the option's own description for ours.
 * @param option The option.
 * @return The translated name.
 */
std::string OptionsFirstPersonState::label(const OptionInfo &option) const
{
	// The game's own frame counter and frame rate limit get our labels.
	if (option.id() == "fpsCounter")
	{
		return std::string(tr("STR_FPS_FRAME_COUNTER"));
	}
	if (option.id() == "FPS")
	{
		return std::string(tr("STR_FPS_FRAME_LIMIT"));
	}
	// An enemy key: "Turn to enemy {0}" with the number from the end of its id.
	const std::string gameKeyLabel = gameKeyName(option.id());
	if (gameKeyLabel == "STR_FPS_KEY_ENEMY")
	{
		return std::string(tr(gameKeyLabel).arg(std::stoi(option.id().substr(strlen(EnemyKeyPrefix)))));
	}
	// Another game key gets our label for it; our own options use their description.
	return std::string(tr(gameKeyLabel.empty() ? option.description() : gameKeyLabel));
}

/**
 * Gives the text for an option's current value: YES or NO for a switch, the number (or its
 * name, for sprite upscaling and an unlimited frame rate) for a number, the key's name for a key.
 * @param option The option.
 * @return The text for the value column.
 */
std::string OptionsFirstPersonState::valueText(const OptionInfo &option) const
{
	switch (option.type())
	{
	case OPTION_BOOL:
		return *option.asBool() ? std::string(tr("STR_YES")) : std::string(tr("STR_NO"));
	case OPTION_INT:
		// Sprite upscaling is a choice of three, shown by name.
		if (option.id() == "fpsSpriteUpscale")
		{
			static const char *const upscaleNames[] = { "STR_FPS_UPSCALE_OFF", "STR_FPS_UPSCALE_SMOOTH", "STR_FPS_UPSCALE_DETAIL" };
			return std::string(tr(upscaleNames[std::clamp(*option.asInt(), 0, 2)]));
		}
		// A frame rate limit of 0 means no limit.
		return option.id() == "FPS" && *option.asInt() == 0 ? std::string(tr("STR_FPS_UNLIMITED")) : std::to_string(*option.asInt());
	case OPTION_KEY:
		return keyName(*option.asKey());
	default:
		return "";
	}
}

/**
 * Lists the settings or the key bindings, depending on the sub-tab. In the key bindings, a
 * heading separates our own keys from the game's keys that work in 3D. Any key waiting to be
 * rebound is forgotten.
 */
void OptionsFirstPersonState::updateList()
{
	_lstOptions->clearList();
	_selectedKey = -1;
	_selKey = nullptr;
	const std::vector<OptionInfo> &shownOptions = _owner == _btnSettings ? _settings : _keys;
	for (size_t optionIndex = 0; optionIndex < shownOptions.size(); ++optionIndex)
	{
		const OptionInfo &option = shownOptions[optionIndex];
		// The heading over the game's keys, before the first of them.
		if (&shownOptions == &_keys && optionIndex == _ownKeys)
		{
			_lstOptions->addRow(1, tr("STR_FPS_GAME_KEYS").c_str());
			_lstOptions->setRowColor(_lstOptions->getLastRowIndex(), _lstOptions->getSecondaryColor());
		}
		// The option's row: its name and its value.
		_lstOptions->addRow(2, label(option).c_str(), valueText(option).c_str());
		if (option.id() == "fpsSoldierPreview")
		{
			_lstOptions->setRowColor(_lstOptions->getLastRowIndex(), _lstOptions->getSecondaryColor()); // The headline feature stands out.
		}
	}
}

/**
 * Gets the option shown on a list row, if any, allowing for the heading row over the game's keys.
 * @param row The list row (as the list counts them, not the lines a wrapped row takes).
 * @param isKey Set to true when the key bindings are showing, false for the settings.
 * @return The option, or nullptr for the heading or a row past the end.
 */
OptionInfo *OptionsFirstPersonState::getEntry(int row, bool *isKey)
{
	std::vector<OptionInfo> &shownOptions = (_owner == _btnSettings) ? _settings : _keys;
	*isKey = (_owner != _btnSettings);
	// Rows from the heading on are one further down than their option.
	if (*isKey && _ownKeys < shownOptions.size() && row >= (int)_ownKeys)
	{
		if (row == (int)_ownKeys) return nullptr; // The heading of the game's keys.
		--row;
	}
	if (row < 0 || row >= (int)shownOptions.size())
	{
		return nullptr;
	}
	return &shownOptions[row];
}

/**
 * Switches between the settings and the key bindings (the button group has already pressed
 * the new sub-tab's button).
 */
void OptionsFirstPersonState::btnListPress(Action *)
{
	updateList();
}

/**
 * Changes the option clicked on. Left click raises a value / toggles / picks a key to rebind;
 * right click lowers a value / clears a key. Each number has its own step and range, and the
 * choices (particles, sprite upscaling) wrap around.
 * @param action The mouse click.
 */
void OptionsFirstPersonState::lstOptionsClick(Action *action)
{
	// Only the left and right buttons do something (not the wheel or the middle button).
	const Uint8 button = action->getDetails()->button.button;
	if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
	{
		return;
	}
	// Any click on the list drops a key that was waiting to be rebound.
	if (_selectedKey != -1)
	{
		_lstOptions->setCellColor(_selectedKey, 0, _colorNormal);
		_lstOptions->setCellColor(_selectedKey, 1, _colorNormal);
		_selectedKey = -1;
		_selKey = nullptr;
	}
	// Find the option clicked on (none for the heading).
	const int row = _lstOptions->getSelectedRow();
	bool isKey = false;
	OptionInfo *option = getEntry(row, &isKey);
	if (!option)
	{
		return;
	}
	// A key: left click marks it to take the next key pressed, right click unbinds it.
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
	// A switch: either button flips it. The frame counter shows or hides at once.
	if (option->type() == OPTION_BOOL)
	{
		*option->asBool() = !*option->asBool();
		if (option->asBool() == &Options::fpsCounter)
		{
			_game->getFpsCounter()->setVisible(Options::fpsCounter);
		}
	}
	// A number: one step up (left) or down (right), within its range.
	else if (option->type() == OPTION_INT)
	{
		int *value = option->asInt();
		const int direction = (button == SDL_BUTTON_LEFT) ? 1 : -1;
		// The field of view: degrees across the screen, in steps of 5 from 50 to 110.
		if (value == &Options::fpsFieldOfView)
		{
			*value = std::clamp(*value + 5 * direction, 50, 110);
		}
		// Mouse sensitivity: 1 to 20.
		else if (value == &Options::fpsMouseSensitivity)
		{
			*value = std::clamp(*value + direction, 1, 20);
		}
		// Walking speed: 1 to 8 tiles per second.
		else if (value == &Options::fpsWalkSpeed)
		{
			*value = std::clamp(*value + direction, 1, 8);
		}
		// Particles (none, effects, everything): round and round, past the last back to the first.
		else if (value == &Options::fpsParticles)
		{
			*value = (*value + direction + 3) % 3;
		}
		// Sprite upscaling (off, smooth, smooth + detail): round and round too.
		else if (value == &Options::fpsSpriteUpscale)
		{
			*value = (*value + direction + 3) % 3;
		}
		// How many voxels the camera sits below the game's eye height: 0 to 6.
		else if (value == &Options::fpsEyeLower)
		{
			*value = std::clamp(*value + direction, 0, 6);
		}
		// Pixel size: 1 to 3.
		else if (value == &Options::fpsPixelSize)
		{
			*value = std::clamp(*value + direction, 1, 3);
		}
		// Time units per level a flying soldier pays: 0 to 24.
		else if (value == &Options::fpsFlightCost)
		{
			*value = std::clamp(*value + direction, 0, 24);
		}
		// The game's frame rate limit, in the steps screens have (0 = unlimited, after the
		// highest). One step up goes to the next step above the current limit, one step down
		// to the next one below it (none: it stays), so a limit that isn't one of the steps
		// (15, or a hand-edited 100) moves to its neighbour.
		else if (value == &Options::FPS)
		{
			static const int frameLimitSteps[] = { 60, 75, 90, 120, 144, 165, 240, 0 };
			const int stepCount = (int)(sizeof(frameLimitSteps) / sizeof(frameLimitSteps[0]));
			auto rate = [](int limit) { return limit == 0 ? 1 << 30 : limit; }; // unlimited is the highest
			const int currentRate = rate(*value);
			if (direction > 0)
			{
				for (int index = 0; index < stepCount; ++index)
				{
					if (rate(frameLimitSteps[index]) > currentRate) { *value = frameLimitSteps[index]; break; }
				}
			}
			else
			{
				for (int index = stepCount - 1; index >= 0; --index)
				{
					if (rate(frameLimitSteps[index]) < currentRate) { *value = frameLimitSteps[index]; break; }
				}
			}
		}
	}
	// Show the new value.
	_lstOptions->setCellText(row, 1, valueText(*option));
}

/**
 * Assigns the pressed key to the binding picked by a left click (modifier keys can't be bound,
 * as in the Controls tab; pressing one just gives up). Either way the binding is no longer
 * waiting for a key.
 * @param action The key press.
 */
void OptionsFirstPersonState::lstOptionsKeyPress(Action *action)
{
	// No binding is waiting for a key.
	if (_selectedKey == -1)
	{
		return;
	}
	// Bind the key, unless it's unknown or a Shift, Alt or Ctrl key.
	const SDLKey key = action->getDetails()->key.keysym.sym;
	if (key != 0 && key != SDLK_LSHIFT && key != SDLK_LALT && key != SDLK_LCTRL &&
		key != SDLK_RSHIFT && key != SDLK_RALT && key != SDLK_RCTRL)
	{
		*_selKey->asKey() = key;
		_lstOptions->setCellText(_selectedKey, 1, keyName(key));
	}
	// The row goes back to its normal colour.
	_lstOptions->setCellColor(_selectedKey, 0, _colorNormal);
	_lstOptions->setCellColor(_selectedKey, 1, _colorNormal);
	_selectedKey = -1;
	_selKey = nullptr;
}

/**
 * Shows the description of the option under the mouse in the tooltip line: our descriptions
 * for the game's two settings and its keys (our label's key plus "_DESC"), and the option's
 * own description key plus "_DESC" for ours. The heading shows nothing. The descriptions are
 * one short line each (the manual explains every setting in full); one too long for the box
 * (a translation, say) is cut short to stay clear of the buttons under it.
 */
void OptionsFirstPersonState::lstOptionsMouseOver(Action *)
{
	bool isKey = false;
	const OptionInfo *option = getEntry(_lstOptions->getSelectedRow(), &isKey);
	std::string tooltip;
	if (option)
	{
		const std::string gameKeyLabel = gameKeyName(option->id());
		tooltip = option->id() == "fpsCounter" ? std::string(tr("STR_FPS_FRAME_COUNTER_DESC"))
			: option->id() == "FPS" ? std::string(tr("STR_FPS_FRAME_LIMIT_DESC"))
			: std::string(tr((gameKeyLabel.empty() ? option->description() : gameKeyLabel) + "_DESC"));
	}
	_txtTooltip->setText(tooltip);

	// Keep the description inside its box: while its lines need more height than the box has,
	// drop its last word (and any punctuation left at the end) and end it in "...". A single
	// word too long for the box is left for the box to clip.
	while (_txtTooltip->getTextHeight() > _txtTooltip->getHeight())
	{
		const size_t lastSpace = tooltip.find_last_of(' ');
		if (lastSpace == std::string::npos)
		{
			break;
		}
		tooltip.erase(lastSpace);
		while (!tooltip.empty() && std::strchr(" ,.;:", tooltip.back()))
		{
			tooltip.pop_back();
		}
		_txtTooltip->setText(tooltip + "...");
	}
}

/**
 * Clears the tooltip line when the mouse leaves the list.
 */
void OptionsFirstPersonState::lstOptionsMouseOut(Action *)
{
	_txtTooltip->setText("");
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
