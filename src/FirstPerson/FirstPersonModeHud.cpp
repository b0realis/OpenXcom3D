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
 * FirstPersonModeHud.cpp: the heads-up display of the first-person mode.
 *
 * The top bar (Iso/3D, HUD, INV, the armor's buttons, one button per soldier, END), the info
 * panel (name, weapon, status, the four stats), the soldier card shown over a squad button
 * (with a live view from their helmet camera), and the banner across the top of the view.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include "FirstPersonView.h"
#include "RuleFirstPerson.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
#include "../Battlescape/Map.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Interface/Bar.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Tells whether this unit can use night vision in the 3D view.
 *
 * With the option "Night vision needs gear" off, everyone has it (as on the isometric map).
 * With it on, night vision comes with gear: an armor or an inventory item that the mod's
 * firstPerson rules list as giving night vision, or eyes that see as far in the dark as by day
 * (aliens, whose view distance is the same by night as by day).
 * @param unit The unit to ask about; null has no night vision.
 * @return True if the unit may switch night vision on.
 */
bool FirstPersonMode::hasNightVision(const BattleUnit *unit) const
{
	// No unit, no eyes.
	if (!unit)
	{
		return false;
	}
	// Gear not required, or the unit sees in the dark by nature.
	if (!Options::fpsNightVisionGear || unit->getMaxViewDistanceAtDark(nullptr) >= unit->getMaxViewDistanceAtDay(nullptr))
	{
		return true;
	}
	// An armor the mod lists as having night vision built in.
	const RuleFirstPerson &rules = _game->getMod()->getFirstPerson();
	if (rules.givesNightVision(unit->getArmor()->getType()))
	{
		return true;
	}
	// Or any item the soldier carries (night vision goggles and the like), wherever it is in the
	// inventory.
	for (const BattleItem *item : *unit->getInventory())
	{
		if (rules.givesNightVision(item->getRules()->getType()))
		{
			return true;
		}
	}
	return false;
}

/**
 * Switches night vision on or off: what the NIGHT VISION button on the top bar and its key do.
 * A soldier without the gear (see hasNightVision) gets a warning instead. The switch itself is
 * the battlescape's own night vision button, so the isometric map and the 3D view share it.
 * @param unit The soldier asking for night vision.
 * @param action The click or key press, handed on to the battlescape's button handler.
 */
void FirstPersonMode::toggleNightVision(BattleUnit *unit, Action *action)
{
	// No gear: say so and leave night vision as it is.
	if (!hasNightVision(unit))
	{
		_state->warning("STR_FPS_NO_NIGHT_VISION");
		return;
	}
	_state->btnNightVisionClick(action);
}

/**
 * Fills the info panel at the bottom left of the 3D view: the soldier's name, the weapon in
 * hand with its ammo, the status line (kneeling, wounds, burning...), and the four stats with
 * their bars (time units, energy, health with the stun level over it, morale).
 * The panel only shows in the 3D view without the classic HUD (which has all of this itself).
 * @param unit The selected soldier, or null for none (the panel hides).
 */
void FirstPersonMode::updateInfoPanel(BattleUnit *unit)
{
	// Show or hide every piece of the panel together. The "doom mode" line shows only while
	// that debug option is on.
	const bool show = isActive() && !_classicHud && unit;
	for (auto *part : _panelParts)
	{
		part->setVisible(show);
	}
	_txtDoom->setVisible(show && Options::fpsDoomMode);
	if (!show)
	{
		return;
	}
	// The three text lines: name, weapon, status.
	_txtName->setText(unit->getName(_game->getLanguage(), false));
	_txtWeapon->setText(weaponLabel(unit));
	_txtStatus->setText(statusLabel(unit));
	// The four stats, each as a number and as a bar measured against the soldier's maximum.
	_numTimeUnits->setValue(unit->getTimeUnits());
	_barTimeUnits->setMax(unit->getBaseStats()->tu);
	_barTimeUnits->setValue(unit->getTimeUnits());
	_numEnergy->setValue(unit->getEnergy());
	_barEnergy->setMax(unit->getBaseStats()->stamina);
	_barEnergy->setValue(unit->getEnergy());
	_numHealth->setValue(std::max(0, unit->getHealth())); // (an overkill leaves it below zero)
	_barHealth->setMax(unit->getBaseStats()->health);
	_barHealth->setValue(std::max(0, unit->getHealth()));
	_barHealth->setValue2(unit->getStunlevel()); // stun damage, drawn over the health bar as in the game
	_numMorale->setValue(unit->getMorale());
	_barMorale->setMax(100);
	_barMorale->setValue(unit->getMorale());
}

/**
 * Makes the weapon line of the info panel and the soldier card: the weapon in the active hand
 * and the rounds left in it, such as "R: Rifle (20)" ("L:" for the left hand). Only firearms
 * show a round count. With dual wield (two guns, both fired together) the line names both:
 * "R+L: Pistol (12|9)" for a matching pair, or "Rifle (20) + Pistol (9)" for two different
 * guns, the chosen hand's gun first.
 * @param unit The soldier whose weapon to describe.
 * @return The weapon line, empty if the soldier's hands are empty.
 */
std::string FirstPersonMode::weaponLabel(BattleUnit *unit) const
{
	std::string text;
	if (BattleItem *weapon = activeWeapon(unit))
	{
		// The rounds left in a weapon as text: its clip's count for a firearm, "-" otherwise.
		auto roundsText = [](const BattleItem *item) {
			const BattleItem *ammo = item->getAmmoForSlot(0);
			return ammo && item->getRules()->getBattleType() == BT_FIREARM ? std::to_string(ammo->getAmmoQuantity()) : std::string("-");
		};
		const std::string name = _state->tr(weapon->getRules()->getName());
		if (const BattleItem *offHand = offHandWeapon(unit, weapon))
		{
			// Dual wield: "R+L: Pistol (12|9)" for a matching pair, or "Rifle (20) + Pistol (9)"
			// (the chosen hand's gun first).
			if (offHand->getRules() == weapon->getRules())
			{
				return "R+L: " + name + " (" + roundsText(weapon) + "|" + roundsText(offHand) + ")";
			}
			return name + " (" + roundsText(weapon) + ") + " + std::string(_state->tr(offHand->getRules()->getName())) + " (" + roundsText(offHand) + ")";
		}
		// One weapon: which hand holds it, its name, and for a loaded firearm its rounds.
		text = std::string(weapon == unit->getLeftHandWeapon() ? "L: " : "R: ") + name;
		const BattleItem *ammo = weapon->getAmmoForSlot(0);
		if (ammo && weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			text += " (" + std::to_string(ammo->getAmmoQuantity()) + ")";
		}
	}
	return text;
}

/**
 * Makes the status line of the info panel and the soldier card: the one thing most worth
 * knowing about the soldier right now. In order: a breakdown this turn (panic or berserk: the
 * soldier is out of the player's hands for the turn), then being on fire, then fatal wounds
 * (they bleed every turn and need a medikit), else kneeling.
 * @param unit The soldier to describe.
 * @return The translated status, or an empty string if there's nothing to say.
 */
std::string FirstPersonMode::statusLabel(BattleUnit *unit) const
{
	// A panic or berserk breakdown in this very turn (remembered per soldier by breakdownCam).
	const auto breakdown = _breakdownTurns.find(unit);
	if (breakdown != _breakdownTurns.end() && breakdown->second.first == _save->getTurn())
	{
		return _state->tr(breakdown->second.second ? "STR_FPS_BERSERK" : "STR_FPS_PANIC");
	}
	// On fire.
	if (unit->getFire() > 0)
	{
		return _state->tr("STR_FPS_BURNING");
	}
	// Bleeding: how many fatal wounds.
	if (unit->getFatalWounds() > 0)
	{
		return _state->tr("STR_FPS_WOUNDS").arg(unit->getFatalWounds());
	}
	// Otherwise only kneeling is worth a word.
	return unit->isKneeled() ? std::string(_state->tr("STR_FPS_KNEELING")) : std::string();
}

/**
 * Keeps the armor's part of the top bar up to date: the armor's name, and a button for each
 * thing the armor and the soldier's gear can do. FLY (SWIM under water) for armor that flies,
 * LIGHT for the armor's personal light, NIGHT VISION for a soldier who has it (see
 * hasNightVision), SPECIAL for a weapon built into the armor. A button shows pressed while its
 * feature is switched on. When a button appears or disappears, the squad buttons are laid out
 * again to make room.
 * @param unit The selected soldier, or null for none (all the buttons hide).
 */
void FirstPersonMode::updateArmorButtons(BattleUnit *unit)
{
	// Which buttons this soldier's armor and gear call for (none outside the 3D view).
	const bool show = isActive() && unit;
	BattleType specialType;
	const bool showFly = show && canFly(unit), showLight = show && unit->getArmor()->getPersonalLightFriend() > 0;
	const bool showSpecial = show && unit->getSpecialIconWeapon(specialType) != nullptr;
	const bool showNight = show && hasNightVision(unit);
	// Show or hide them; any change moves the other buttons on the bar.
	if (showFly != _btnFly->getVisible() || showLight != _btnLight->getVisible()
		|| showNight != _btnNight->getVisible() || showSpecial != _btnSpecial->getVisible())
	{
		_btnFly->setVisible(showFly);
		_btnLight->setVisible(showLight);
		_btnNight->setVisible(showNight);
		_btnSpecial->setVisible(showSpecial);
		layoutTopBar(); // the squad buttons make room
	}
	if (!show)
	{
		return;
	}
	// The armor's name ("no armor" for the game's placeholder armor).
	const std::string &armorType = unit->getArmor()->getType();
	_txtArmor->setText(armorType == "STR_NONE_UC" ? _state->tr("STR_FPS_NO_ARMOR") : _state->tr(armorType));
	// A toggle button shows pressed when its group pointer points at it. Work out which are on,
	// and redraw the buttons only when that changed.
	TextButton *flyGroup = flightOn(unit) ? _btnFly : nullptr;
	TextButton *lightGroup = _save->getTogglePersonalLight() ? _btnLight : nullptr;
	TextButton *nightGroup = _state->_map->isNightVisionOn() ? _btnNight : nullptr;
	if (flyGroup != _flyButtonGroup || lightGroup != _lightButtonGroup || nightGroup != _nightButtonGroup)
	{
		_flyButtonGroup = flyGroup;
		_lightButtonGroup = lightGroup;
		_nightButtonGroup = nightGroup;
		_btnFly->invalidate();
		_btnLight->invalidate();
		_btnNight->invalidate();
	}
}

/**
 * Shows the soldier card under the squad button the mouse is over: the soldier's name, status,
 * weapon and the four stats with their bars. With the soldier preview option on, the card also
 * shows a live picture from the soldier's helmet camera (a small second 3D view), or TV static
 * if the soldier is dead or unconscious. The card hides when no squad button is hovered, or
 * while the mouse is captured for mouse look.
 */
void FirstPersonMode::updateSoldierCard()
{
	// The soldier under the mouse, if any (only a visible button counts, and only with a free
	// cursor).
	BattleUnit *unit = nullptr;
	if (_hoveredSquadButton >= 0 && _hoveredSquadButton < (int)_btnSquad.size() && _btnSquad[_hoveredSquadButton]->getVisible() && !wantsMouseCapture())
	{
		unit = _squadUnits[_hoveredSquadButton];
	}
	const SquadState state = unit ? squadState(unit) : SQUAD_NONE;
	// The dead and the unconscious: their helmet camera shows only static.
	const bool live = unit && unit->getTile() && (state == SQUAD_READY || state == SQUAD_CONTROLLED);
	if (state == SQUAD_NONE)
	{
		unit = nullptr;
	}
	// Show the card's parts: the camera view only for a live soldier, the "no signal" text only
	// for a dead camera, and both only with the preview option.
	const bool preview = unit && Options::fpsSoldierPreview;
	for (const auto &part : _cardParts)
	{
		part.surface->setVisible(unit && (part.surface != _preview || (preview && live)) && (part.surface != _cardSignal || (preview && !live)));
	}
	if (!unit)
	{
		return;
	}
	// Place the card centred under its button, kept inside the canvas.
	const TextButton *button = _btnSquad[_hoveredSquadButton];
	const int cardX = std::clamp(button->getX() + button->getWidth() / 2 - CardW / 2, 1, canvasWidth() - CardW - 1);
	for (const auto &part : _cardParts)
	{
		part.surface->setX(cardX + part.dx);
		part.surface->setY(CardY + part.dy);
	}
	// Redraw the card's frame and background only when its height changes (taller with the
	// camera view). The colours come from the mod's "firstPerson" interface, element "panel",
	// if it sets them.
	const int cardHeight = preview ? 36 + PreviewH + 4 : 35;
	if (cardHeight != _cardDrawnHeight)
	{
		_cardDrawnHeight = cardHeight;
		Uint8 background = 15, border = 8;
		if (const RuleInterface *interfaceRules = _game->getMod()->getInterface("firstPerson", false))
		{
			if (const Element *panelElement = interfaceRules->getElementOptional("panel"))
			{
				if (panelElement->color != INT_MAX) background = panelElement->color;
				if (panelElement->border != INT_MAX) border = panelElement->border;
			}
		}
		_card->clear();
		_card->drawRect(0, 0, CardW, cardHeight, border);
		_card->drawRect(1, 1, CardW - 2, cardHeight - 2, background);
		if (preview)
		{
			_card->drawRect(3, 35, PreviewW + 2, PreviewH + 2, border); // frame around the camera
		}
	}
	// The text: name, weapon, and a status that says first whether the soldier has fallen or
	// been turned.
	_cardName->setText(unit->getName(_game->getLanguage(), false));
	_cardWeapon->setText(weaponLabel(unit));
	const std::string status = state == SQUAD_DEAD ? std::string(_state->tr("STR_FPS_DECEASED"))
		: state == SQUAD_ZOMBIE ? std::string(_state->tr("STR_FPS_ZOMBIE"))
		: state == SQUAD_UNCONSCIOUS ? std::string(_state->tr("STR_FPS_UNCONSCIOUS"))
		: state == SQUAD_CONTROLLED ? std::string(_state->tr("STR_FPS_MIND_CONTROLLED")) : statusLabel(unit);
	_cardStatus->setText(status);
	// The name gives way to the status beside it.
	const int nameWidth = CardW - 8 - (status.empty() ? 0 : _cardStatus->getTextWidth() + 4);
	if (nameWidth != _cardName->getWidth())
	{
		_cardName->setWidth(nameWidth);
	}
	if (preview && !live)
	{
		// Static on the dead camera: every pixel a random grey, new every frame (a linear
		// congruential generator, its top bits picking one of four greys).
		const SDL_Color *palette = _state->getPalette();
		const Uint8 greys[4] = { nearestColor(palette, 20, 20, 20), nearestColor(palette, 70, 70, 70),
			nearestColor(palette, 130, 130, 130), nearestColor(palette, 200, 200, 200) };
		_card->lock();
		for (int noiseY = 0; noiseY < PreviewH; ++noiseY)
		{
			for (int noiseX = 0; noiseX < PreviewW; ++noiseX)
			{
				_cardNoiseSeed = _cardNoiseSeed * 1664525u + 1013904223u;
				_card->setPixel(4 + noiseX, 36 + noiseY, greys[(_cardNoiseSeed >> 28) & 3]);
			}
		}
		_card->unlock();
	}
	// The four stats (time units, energy, health, morale) as numbers and bars, with the stun
	// level over the health bar.
	// (an overkill leaves health below zero)
	const int values[4] = { unit->getTimeUnits(), unit->getEnergy(), std::max(0, unit->getHealth()), unit->getMorale() };
	const int maxima[4] = { unit->getBaseStats()->tu, unit->getBaseStats()->stamina, unit->getBaseStats()->health, 100 };
	for (int stat = 0; stat < 4; ++stat)
	{
		_cardNumbers[stat]->setValue(values[stat]);
		_cardBars[stat]->setMax(maxima[stat]);
		_cardBars[stat]->setValue(values[stat]);
	}
	_cardBars[2]->setValue2(unit->getStunlevel());

	if (preview && live)
	{
		// Their eyes. When it's the soldier we're looking through, copy the 3D camera itself
		// (its eye, and its yaw by turning the preview by the difference), level.
		_preview->setCameraUnit(unit);
		if (isActive() && unit == _trackedUnit)
		{
			double eye[3];
			_view->getEye(eye);
			_preview->setEye(eye[0], eye[1], eye[2]);
			_preview->lookAlong(unit->getDirection());
			_preview->rotate(_view->getYawDegrees() - _preview->getYawDegrees(), 0.0);
		}
		else
		{
			// Anyone else: an eye in the middle of the soldier's tile (or 2x2 tiles for a large
			// unit) at their eye height, looking the way they face. Voxels: a tile is 16 across.
			const Position position = unit->getPosition();
			const int unitSize = unit->getArmor()->getSize();
			_preview->setEye(position.x * 16.0 + 8.0 * unitSize, position.y * 16.0 + 8.0 * unitSize, eyeHeight(unit));
			_preview->lookAlong(unit->getDirection());
		}
		// The same view settings as the main 3D view, and night vision if the soldier has it.
		_preview->setFieldOfView(Options::fpsFieldOfView);
		_preview->setSmoothLighting(Options::fpsSmoothLighting);
		_preview->setNightVision(hasNightVision(unit) ? _state->_map->getNightVisionColor() : 0);
		_preview->invalidate();
	}
}

/**
 * Keeps the banner across the top of the 3D view up to date. It says, in order of importance:
 * BURNING for a moment after the soldier catches fire, ENEMY SPOTTED for a moment after a new
 * enemy comes into sight in the player's turn, or HIDDEN MOVEMENT during the aliens' turn while
 * no enemy is in sight (as the isometric map does). The letters glow and pulse; the alarms
 * (burning, enemy spotted) pulse faster and in red.
 * @param playerTurn True in the player's turn.
 */
void FirstPersonMode::updateBanner(bool playerTurn)
{
	// Pick the text: the first that applies, or none.
	std::string text;
	const bool burning = isActive() && SDL_GetTicks() < _burningBannerUntil;
	if (burning)
	{
		text = _state->tr("STR_FPS_BURNING");
	}
	else if (isActive() && playerTurn && SDL_GetTicks() < _spottedUntil)
	{
		text = _state->tr("STR_FPS_ENEMY_SPOTTED");
	}
	else if (isActive() && !playerTurn)
	{
		// Hidden movement, unless an enemy can be seen (then the view itself shows them).
		text = _state->tr("STR_HIDDEN_MOVEMENT");
		for (BattleUnit *unit : *_save->getUnits())
		{
			if (!unit->isOut() && unit->getFaction() == FACTION_HOSTILE && unit->getVisible())
			{
				text.clear();
				break;
			}
		}
	}
	// Set the text on the banner and on the glow copies around it, only when it changed.
	if (!text.empty() && _txtBanner->getText() != text)
	{
		_txtBanner->setText(text);
		for (Text *glow : _bannerGlow)
		{
			glow->setText(text);
		}
	}
	// Show the banner and its glow only with something to say.
	_txtBanner->setVisible(!text.empty());
	for (Text *glow : _bannerGlow)
	{
		glow->setVisible(!text.empty());
	}
	if (text.empty())
	{
		return;
	}
	// Glowing letters: a yellow core that pulses towards orange, in a halo going red to purple
	// (enemy spotted and burning pulse faster, in plain red). The pulse is a sine of the time:
	// one radian every 160 ms for an alarm, every 450 ms otherwise.
	const SDL_Color *palette = _game->getScreen()->getPalette();
	const bool alarm = playerTurn || burning;
	const double pulsePhase = SDL_GetTicks() / (alarm ? 160.0 : 450.0);
	const double pulse = 0.5 + 0.5 * std::sin(pulsePhase);
	const Uint8 core = pulse > 0.5 ? textColor(palette, 255, 230, 70) : textColor(palette, 255, 170, 40);
	const Uint8 halo = alarm ? textColor(palette, 240, 40, 30)
		: (std::sin(pulsePhase * 0.37) > 0 ? textColor(palette, 235, 50, 40) : textColor(palette, 190, 70, 230));
	// Recolour only on change (setting a colour redraws the text).
	if (_txtBanner->getColor() != core)
	{
		_txtBanner->setColor(core);
	}
	for (Text *glow : _bannerGlow)
	{
		if (glow->getColor() != halo) glow->setColor(halo);
	}
	if (_textShadows.front()->getColor() != halo) _textShadows.front()->setColor(halo); // the banner's shadow is part of the halo
}

/**
 * Works out which unit each squad button stands for: the player's own units in a stable order
 * (so the numbers don't shift when someone falls), then any aliens under the player's mind
 * control. Makes more buttons (each with an icon surface) when the squad has grown, and updates
 * each button's look when its soldier's state changed: a fallen or turned soldier's button
 * shows an icon instead of the number, in a colour of its own. Lays the bar out again if
 * anything changed.
 */
void FirstPersonMode::refreshSquad()
{
	// Everyone who came with the player (the fallen too, so their button keeps its place),
	// then the aliens the player controls right now.
	std::vector<BattleUnit*> squad;
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getOriginalFaction() == FACTION_PLAYER)
		{
			squad.push_back(unit);
		}
	}
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getOriginalFaction() != FACTION_PLAYER && unit->getFaction() == FACTION_PLAYER && !unit->isOut())
		{
			squad.push_back(unit);
		}
	}
	// Make a button, an icon and a state slot for every unit beyond the buttons there are
	// (buttons are never removed; spare ones stay hidden).
	while (_btnSquad.size() < squad.size())
	{
		TextButton *button = makeButton(SquadX, 16, std::to_string(_btnSquad.size() + 1));
		button->setGroup(&_squadButtonGroup);
		button->setVisible(false);
		_squadColor = button->getColor();
		_btnSquad.push_back(button);
		Surface *icon = new Surface(7, 8, 0, BarY + 2);
		_state->add(icon);
		icon->setVisible(false);
		_ownSurfaces.insert(icon);
		_squadIcons.push_back(icon);
		_squadStates.push_back(SQUAD_NONE);
	}
	// One unit per button (null for the spare ones).
	squad.resize(_btnSquad.size(), nullptr);
	bool changed = squad != _squadUnits;
	_squadUnits = squad;
	// How each one is: a fallen or turned soldier's button shows an icon instead of the number.
	// A button's colour is the base of a 16-colour group (TextButton shades from it), so pick
	// the group whose fourth shade comes nearest to the wanted colour (weighted distance, green
	// counting most, as the eye sees it).
	const SDL_Color *palette = _state->getPalette();
	auto colorGroupNear = [palette](int red, int green, int blue) {
		int best = 0, bestDist = INT_MAX;
		for (int group = 0; group < 16; ++group)
		{
			const SDL_Color &sample = palette[group * 16 + 3];
			const int dist = (sample.r - red) * (sample.r - red) * 3 + (sample.g - green) * (sample.g - green) * 6 + (sample.b - blue) * (sample.b - blue);
			if (dist < bestDist) { bestDist = dist; best = group; }
		}
		return (Uint8)(best * 16);
	};
	// Restyle the buttons whose soldier's state changed: number or icon, and the colour
	// (orange: mind controlled, grey: dead, blue: unconscious, green: zombie).
	for (size_t slot = 0; slot < _squadUnits.size(); ++slot)
	{
		const SquadState state = squadState(_squadUnits[slot]);
		if (state == _squadStates[slot]) continue;
		_squadStates[slot] = state;
		changed = true;
		TextButton *button = _btnSquad[slot];
		button->setText(state == SQUAD_READY ? std::to_string(slot + 1) : std::string());
		button->setColor(state == SQUAD_CONTROLLED ? colorGroupNear(230, 130, 30) : state == SQUAD_DEAD ? colorGroupNear(120, 120, 120)
			: state == SQUAD_UNCONSCIOUS ? colorGroupNear(90, 120, 170) : state == SQUAD_ZOMBIE ? colorGroupNear(90, 140, 60) : _squadColor);
		drawSquadIcon(_squadIcons[slot], state);
	}
	if (changed)
	{
		layoutTopBar();
	}
}

/**
 * Lays out the right part of the top bar (canvas pixels). From the right: END, then the
 * armor's buttons (with long labels, or short ones if the bar is too narrow for them), then
 * the squad buttons between the previous and next buttons. The squad buttons take what room
 * is left: at most ten are shown at once (a window that scrolls over the rest), each at least
 * SquadMinW and at most 18 pixels wide. Each button keeps its slot's number.
 */
void FirstPersonMode::layoutTopBar()
{
	// The slots with a button: every soldier we came with (the fallen too), and aliens under
	// our control. Hide all the buttons for now; the ones in the window are shown below.
	_squadSlots.clear();
	for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
	{
		const SquadState state = buttonIndex < _squadStates.size() ? _squadStates[buttonIndex] : SQUAD_NONE;
		_btnSquad[buttonIndex]->setVisible(false);
		_squadIcons[buttonIndex]->setVisible(false);
		if (state != SQUAD_NONE) _squadSlots.push_back(buttonIndex);
	}
	const int wantedButtons = std::min<int>(SquadWindow, (int)_squadSlots.size());
	// The suit's buttons, right to left from END. A first pass tries the long labels; if that
	// leaves too little room for the squad buttons, a second pass uses the short ones.
	// freeRight ends up as the right end of the room left for the squad buttons.
	TextButton *suitButtons[4] = { _btnFly, _btnLight, _btnNight, _btnSpecial };
	const int squadLeft = SquadX + PrevNextW + 1;
	int freeRight = 0;
	for (int pass = 0; pass < 2; ++pass)
	{
		freeRight = canvasWidth() - 1 - EndTurnW - 2;
		for (int suitIndex = 0; suitIndex < 4; ++suitIndex)
		{
			if (!suitButtons[suitIndex]->getVisible()) continue;
			const std::string &label = pass == 0 ? _suitLabelsLong[suitIndex] : _suitLabelsShort[suitIndex];
			suitButtons[suitIndex]->setText(label);
			suitButtons[suitIndex]->setWidth(labelWidth(label));
			freeRight -= suitButtons[suitIndex]->getWidth();
			suitButtons[suitIndex]->setX(freeRight);
			freeRight -= 1;
		}
		if (freeRight - 1 - PrevNextW - squadLeft >= wantedButtons * (SquadMinW + 1)) break;
	}
	// Up to ten squad buttons between previous and next; the window scrolls. Work out how many
	// fit, keep the scroll position inside the slots, and share out the width.
	const int availableWidth = freeRight - 1 - PrevNextW - squadLeft;
	_squadWindow = std::clamp(availableWidth / (SquadMinW + 1), 1, std::max(1, wantedButtons));
	_squadScroll = std::clamp(_squadScroll, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	const int buttonWidth = std::clamp(availableWidth / _squadWindow - 1, SquadMinW, 18);
	// Place previous, the buttons in the window (each with its icon centred on it, shown for a
	// soldier who isn't ready), and next right after them.
	_btnPrev->setX(SquadX);
	int buttonX = squadLeft;
	for (int slotIndex = _squadScroll; slotIndex < std::min((int)_squadSlots.size(), _squadScroll + _squadWindow); ++slotIndex)
	{
		const size_t buttonIndex = _squadSlots[slotIndex];
		_btnSquad[buttonIndex]->setX(buttonX);
		_btnSquad[buttonIndex]->setWidth(buttonWidth);
		_btnSquad[buttonIndex]->setVisible(true);
		_squadIcons[buttonIndex]->setX(buttonX + (buttonWidth - 7) / 2);
		_squadIcons[buttonIndex]->setVisible(_squadStates[buttonIndex] != SQUAD_READY);
		buttonX += buttonWidth + 1;
	}
	_btnNext->setX(buttonX);
}

/**
 * Scrolls the window of squad buttons by some slots (the mouse wheel over the bar, or following
 * the selected soldier), kept within the slots there are.
 * @param step Slots to scroll: positive towards the later slots, negative towards the first.
 */
void FirstPersonMode::scrollSquad(int step)
{
	const int newScroll = std::clamp(_squadScroll + step, 0, std::max(0, (int)_squadSlots.size() - _squadWindow));
	if (newScroll != _squadScroll)
	{
		_squadScroll = newScroll;
		layoutTopBar();
	}
}

/**
 * Tells how a squad member is: ready, dead, a zombie (killed and turned into one), unconscious,
 * or turned against us by the aliens' mind control. An alien we control is ready while that
 * lasts, and has no button otherwise.
 * @param unit The unit of a squad button, or null.
 * @return Its state; SQUAD_NONE means it gets no button.
 */
FirstPersonMode::SquadState FirstPersonMode::squadState(const BattleUnit *unit) const
{
	// No unit, or one not in this battle.
	if (!unit || unit->isIgnored())
	{
		return SQUAD_NONE; // (left behind in an earlier stage of the mission: not in this battle)
	}
	// An alien: on the squad bar only while under our control and standing.
	if (unit->getOriginalFaction() != FACTION_PLAYER)
	{
		return unit->getFaction() == FACTION_PLAYER && !unit->isOut() ? SQUAD_READY : SQUAD_NONE;
	}
	// Our own soldier, dead (killed outright, or down with no health left).
	if (unit->getStatus() == STATUS_DEAD || (unit->isOut() && unit->getHealth() <= 0))
	{
		return unit->getAlreadyRespawned() ? SQUAD_ZOMBIE : SQUAD_DEAD; // (turned into a zombie when killed)
	}
	// Down but alive: unconscious.
	if (unit->isOut())
	{
		return SQUAD_UNCONSCIOUS;
	}
	// Standing: ours, or the aliens' while under their mind control.
	return unit->getFaction() == FACTION_PLAYER ? SQUAD_READY : SQUAD_CONTROLLED;
}

/**
 * Draws the pixel-art icon (7x8 pixels) that replaces a squad button's number: a skull for the
 * dead (bone white, rotting green for a zombie), a caduceus for the unconscious (a gold staff
 * with green snakes), and an alien face for a soldier under the aliens' mind control (grey
 * skin, black eyes). Other states leave the icon empty.
 * @param icon The icon surface to draw on.
 * @param state The squad member's state.
 */
void FirstPersonMode::drawSquadIcon(Surface *icon, SquadState state)
{
	// The pictures, one string per row: '#' the main ink, 'o' the second ink, '.' transparent.
	static const char *skull[8] = { ".#####.", "#######", "#..#..#", "#..#..#", "###.###", ".#####.", ".#.#.#.", "..###.." };
	static const char *caduceus[8] = { "##.#.##", ".#####.", "...#...", ".##o##.", "o..#..o", ".oo#oo.", "...#...", "..###.." };
	static const char *alien[8] = { "..###..", ".#####.", "#######", "#oo#oo#", "#.o#o.#", ".#####.", "..###..", "...#..." };
	// Pick the picture and its two inks for the state, as the palette's nearest colours.
	const SDL_Color *palette = _state->getPalette();
	const char **art = state == SQUAD_DEAD || state == SQUAD_ZOMBIE ? skull
		: state == SQUAD_UNCONSCIOUS ? caduceus : state == SQUAD_CONTROLLED ? alien : nullptr;
	Uint8 ink = 0, secondInk = 0;
	switch (state)
	{
	case SQUAD_DEAD: ink = nearestColor(palette, 235, 230, 205); break;                                    // bone
	case SQUAD_ZOMBIE: ink = nearestColor(palette, 150, 210, 90); break;                                   // rotting green
	case SQUAD_UNCONSCIOUS: ink = nearestColor(palette, 245, 205, 70); secondInk = nearestColor(palette, 120, 230, 140); break; // gold staff, green snakes
	case SQUAD_CONTROLLED: ink = nearestColor(palette, 180, 200, 175); secondInk = nearestColor(palette, 10, 10, 10); break;   // grey skin, black eyes
	default: break;
	}
	icon->clear();
	if (!art)
	{
		return;
	}
	// Copy the picture onto the icon, pixel by pixel.
	icon->lock();
	for (int iconY = 0; iconY < 8; ++iconY)
	{
		for (int iconX = 0; iconX < 7; ++iconX)
		{
			if (art[iconY][iconX] == '#') icon->setPixel(iconX, iconY, ink);
			else if (art[iconY][iconX] == 'o') icon->setPixel(iconX, iconY, secondInk);
		}
	}
	icon->unlock();
}

/**
 * Keeps the top bar in sync with the game every frame: the squad buttons (see refreshSquad),
 * the window of squad buttons scrolled to show the selected soldier, the selected soldier's
 * button pressed, the HUD button pressed while the classic HUD is on, the HUD and END buttons
 * only in the 3D view, and the view button saying where a click takes you (Iso or 3D).
 * @param selected The selected unit, or null.
 */
void FirstPersonMode::updateTopBar(BattleUnit *selected)
{
	refreshSquad();
	// The window of squad buttons follows the selected soldier: when another soldier is
	// selected, scroll just far enough to bring their button into the window.
	if (selected != _squadScrolledTo)
	{
		_squadScrolledTo = selected;
		for (int slotIndex = 0; slotIndex < (int)_squadSlots.size(); ++slotIndex)
		{
			if (_squadUnits[_squadSlots[slotIndex]] != selected) continue;
			if (slotIndex < _squadScroll) scrollSquad(slotIndex - _squadScroll);
			else if (slotIndex >= _squadScroll + _squadWindow) scrollSquad(slotIndex - _squadScroll - _squadWindow + 1);
		}
	}

	// The selected soldier's button shows pressed (the group pointer points at it); redraw the
	// buttons when that changes.
	TextButton *squadGroup = nullptr;
	for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
	{
		if (_squadUnits[buttonIndex] && _squadUnits[buttonIndex] == selected)
		{
			squadGroup = _btnSquad[buttonIndex];
		}
	}
	if (squadGroup != _squadButtonGroup)
	{
		_squadButtonGroup = squadGroup;
		for (auto *button : _btnSquad)
		{
			button->invalidate();
		}
	}

	// The HUD button: pressed while the classic HUD is drawn, shown only in 3D. END too shows
	// only in 3D, and the view button names the view a click switches to.
	const bool active = isActive();
	TextButton *hudGroup = _classicHud ? _btnHud : nullptr;
	if (hudGroup != _hudButtonGroup)
	{
		_hudButtonGroup = hudGroup;
		_btnHud->invalidate();
	}
	_btnHud->setVisible(active);
	_btnEndTurn->setVisible(active); // the classic HUD has its own
	_btnView->setText(_state->tr(active ? "STR_FPS_BUTTON_ISO" : "STR_FPS_BUTTON_3D"));
}

/**
 * Finds which of our top bar buttons is under a point, if any. Only visible buttons count.
 * @param x The point's x, in canvas pixels.
 * @param y The point's y, in canvas pixels.
 * @return The button under the point, or null.
 */
TextButton *FirstPersonMode::buttonAt(double x, double y) const
{
	// Is the point inside this button's rectangle (and is the button showing)?
	auto inside = [&](const TextButton *button) {
		return button->getVisible() && x >= button->getX() && x < button->getX() + button->getWidth()
			&& y >= button->getY() && y < button->getY() + button->getHeight();
	};
	// The fixed buttons first, then the squad buttons.
	for (TextButton *button : { _btnView, _btnHud, _btnInventory, _btnEndTurn, _btnFly, _btnLight, _btnNight, _btnSpecial, _btnPrev, _btnNext })
	{
		if (inside(button)) return button;
	}
	for (TextButton *button : _btnSquad)
	{
		if (inside(button)) return button;
	}
	return nullptr;
}

/**
 * Acts on a click on one of our top bar buttons. Most hand the click to the battlescape's own
 * button of the same job (inventory, end turn, personal light, special weapon, previous and
 * next soldier), so the game's rules for it apply; the view, HUD, flight and night vision
 * buttons are ours, and a squad button selects its soldier.
 * @param button The button clicked.
 * @param action The click, handed on to the battlescape's handlers.
 */
void FirstPersonMode::buttonClicked(TextButton *button, Action *action)
{
	// One branch per button (the squad buttons last).
	if (button == _btnView)
	{
		toggleView();
	}
	else if (button == _btnHud)
	{
		setClassicHud(!_classicHud);
	}
	else if (button == _btnInventory)
	{
		_state->btnInventoryClick(action);
	}
	else if (button == _btnEndTurn)
	{
		_state->btnEndTurnClick(action);
	}
	else if (button == _btnFly)
	{
		toggleFlight(_save->getSelectedUnit());
	}
	else if (button == _btnLight)
	{
		_state->btnPersonalLightingClick(action);
	}
	else if (button == _btnNight)
	{
		toggleNightVision(_save->getSelectedUnit(), action);
	}
	else if (button == _btnSpecial)
	{
		_state->btnSpecialClick(action);
	}
	else if (button == _btnPrev)
	{
		_state->btnPrevSoldierClick(action);
	}
	else if (button == _btnNext)
	{
		_state->btnNextSoldierClick(action);
	}
	else
	{
		// A squad button: select the soldier it stands for (buttons of empty slots do nothing).
		for (size_t buttonIndex = 0; buttonIndex < _btnSquad.size(); ++buttonIndex)
		{
			if (button == _btnSquad[buttonIndex] && _squadUnits[buttonIndex])
			{
				selectUnit(_squadUnits[buttonIndex]);
			}
		}
	}
}

/**
 * Selects a soldier from the squad bar, the same way the battlescape's next-soldier button
 * does: the isometric camera centres on them, any action in progress is cancelled, and the
 * cursor is set up for the new soldier. Nothing happens while the battlescape's buttons are
 * locked (the aliens' turn, an action running, a shot in flight, a panic not yet dealt with),
 * or for a soldier who is down or not under the player's control.
 * @param unit The soldier to select.
 */
void FirstPersonMode::selectUnit(BattleUnit *unit)
{
	// Only a soldier the player can command, and only when the game takes orders.
	if (!_state->allowButtons() || unit->isOut() || unit->getFaction() != FACTION_PLAYER)
	{
		return;
	}
	// Select them, as BattlescapeState's next-soldier button does.
	_save->setSelectedUnit(unit);
	_state->updateSoldierInfo();
	_state->_map->getCamera()->centerOnPosition(unit->getPosition());
	_state->_battleGame->cancelAllActions();
	_state->_battleGame->getCurrentAction()->actor = unit;
	_state->_battleGame->setupCursor();
}

/**
 * Hands the 3D view the visible enemies with their numbers, so it marks them. The numbers are
 * those of the classic HUD's red buttons on the right (one per enemy in sight), so a marker in
 * the 3D view and a button on the classic HUD always agree.
 */
void FirstPersonMode::updateEnemyMarkers()
{
	// Every red button showing, with a standing enemy behind it, numbered from 1.
	std::vector<std::pair<const BattleUnit*, int>> markers;
	for (int buttonIndex = 0; buttonIndex < BattlescapeState::VISIBLE_MAX; ++buttonIndex)
	{
		BattleUnit *enemy = _state->_visibleUnit[buttonIndex];
		if (enemy && !enemy->isOut() && _state->_btnVisibleUnit[buttonIndex]->getVisible())
		{
			markers.push_back({ enemy, buttonIndex + 1 });
		}
	}
	_view->setMarkers(markers);
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
