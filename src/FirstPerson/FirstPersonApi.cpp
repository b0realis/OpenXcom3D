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
 * FirstPersonApi.cpp: the bridge between the core engine and the add-on. Each function the core
 * calls (see FirstPersonApi.h) is passed on to FirstPersonMode, or registers the add-on's options
 * and its 3D VIEW options tab.
 */
#include "FirstPersonApi.h"
#include "FirstPersonMode.h"
#include "OptionsFirstPersonState.h"
#include "../Engine/Game.h"
#include "../Engine/OptionInfo.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"

namespace OpenXcom
{

namespace FirstPerson
{

/**
 * Creates the first-person mode for a battlescape screen, but only when an active mod switches
 * the add-on on (`firstPerson: enabled: true`). Without such a mod the battlescape gets no mode
 * at all, and the calls below that take the mode do nothing, so the game plays exactly as
 * without the add-on.
 * @param game The game.
 * @param state The battlescape screen the mode belongs to.
 * @return The new mode, or nullptr when no active mod enables the add-on.
 */
FirstPersonMode *create(Game *game, BattlescapeState *state)
{
	if (!game->getMod()->getFirstPerson().isEnabled())
	{
		return nullptr;
	}
	return new FirstPersonMode(game, state);
}

/**
 * Destroys the first-person mode when its battlescape screen closes (nullptr is fine).
 * @param mode The mode, or nullptr.
 */
void destroy(FirstPersonMode *mode)
{
	delete mode;
}

/**
 * Gives the first-person mode its per-frame update (movement, camera, the 3D picture), if
 * there is a mode.
 * @param mode The mode, or nullptr.
 */
void think(FirstPersonMode *mode)
{
	if (mode)
	{
		mode->think();
	}
}

/**
 * Gives the first-person mode the first look at an input event (keys, mouse), before the
 * battlescape screen handles it.
 * @param mode The mode, or nullptr.
 * @param action The event.
 * @return True if the mode used the event up (the battlescape screen then ignores it).
 */
bool handle(FirstPersonMode *mode, Action *action)
{
	return mode && mode->handle(action);
}

/**
 * Lets the first-person mode draw the battlescape screen its own way (without the classic HUD
 * when that is hidden).
 * @param mode The mode, or nullptr.
 * @return True if the mode drew the screen; false to let the battlescape screen draw it normally.
 */
bool blit(FirstPersonMode *mode)
{
	return mode && mode->blit();
}

/**
 * Asks whether the mouse should be captured right now, so moving it turns the view (mouse look).
 * @param mode The mode, or nullptr.
 * @return True while the 3D view wants mouse look; always false without a mode.
 */
bool wantsMouseCapture(const FirstPersonMode *mode)
{
	return mode && mode->wantsMouseCapture();
}

/**
 * Tells the first-person mode that the battlescape screen was resized. The screen has already
 * moved all its surfaces (its HUD to the middle and the bottom), ours among them; the mode puts
 * its own back where they belong and remakes the 3D view at the new size.
 * @param mode The mode, or nullptr.
 * @param widthChange How much wider the screen got, in pixels (negative when narrower).
 * @param heightChange How much taller the screen got, in pixels (negative when shorter).
 */
void resized(FirstPersonMode *mode, int widthChange, int heightChange)
{
	if (mode)
	{
		mode->resized(widthChange, heightChange);
	}
}

/**
 * Registers the add-on's options with the game's (called from Options::createOptionsOTHER, the
 * engine's place for a fork's own options), so they are loaded, saved and reset to their
 * defaults like any other option. Category STR_FIRST_PERSON is only listed by the 3D VIEW tab,
 * so they stay out of the other option screens; the tab lists them in the order they are
 * added here.
 * @param options The game's list of options, which they are added to.
 */
void createOptions(std::vector<OptionInfo> &options)
{
	using namespace Options;
	const char *category = "STR_FIRST_PERSON";

	// The settings (YES/NO switches and numbers), in the order the Settings list shows them.
	// First the headline feature: hovering a soldier's button shows that soldier's helmet camera.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSoldierPreview", &fpsSoldierPreview, true, "STR_FPS_SOLDIER_PREVIEW", category));
	// Mouse look: how fast the view turns (1 to 20), up and down swapped or not, and the field
	// of view across the screen, in degrees (50 to 110).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMouseSensitivity", &fpsMouseSensitivity, 5, "STR_FPS_MOUSE_SENSITIVITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsInvertMouse", &fpsInvertMouse, false, "STR_FPS_INVERT_MOUSE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFieldOfView", &fpsFieldOfView, 75, "STR_FPS_FIELD_OF_VIEW", category));
	// Moving: walking speed in tiles per second (1 to 8; every tile still costs its time units),
	// the view bobbing with the steps, time units a flying soldier pays per level up or down
	// (0 = free), jumping over low obstacles, falling off ledges, and how many voxels below the
	// game's eye height the camera sits (0 to 6).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsWalkSpeed", &fpsWalkSpeed, 4, "STR_FPS_WALK_SPEED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHeadBob", &fpsHeadBob, true, "STR_FPS_HEAD_BOB", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCost", &fpsFlightCost, 0, "STR_FPS_FLIGHT_COST", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVault", &fpsVault, true, "STR_FPS_VAULT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFalling", &fpsFalling, true, "STR_FPS_FALLING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEyeLower", &fpsEyeLower, 3, "STR_FPS_EYE_LOWER", category));
	// Shooting: the game's own accuracy (YES) or FPS-style shooting along the crosshair (NO), and
	// firing a gun in each hand at once.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFaithfulTargeting", &fpsFaithfulTargeting, true, "STR_FPS_FAITHFUL_TARGETING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDualWield", &fpsDualWield, false, "STR_FPS_DUAL_WIELD", category));
	// Light and atmosphere: smooth lighting between tiles, ray-traced shadows, the personal light
	// as a shoulder flashlight, night vision only with the right gear, tenser music while aliens
	// are in sight, and unlimited tactical flares.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothLighting", &fpsSmoothLighting, true, "STR_FPS_SMOOTH_LIGHTING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsShadows", &fpsShadows, true, "STR_FPS_SHADOWS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlight", &fpsFlashlight, true, "STR_FPS_FLASHLIGHT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionGear", &fpsNightVisionGear, true, "STR_FPS_NIGHT_VISION_GEAR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCombatMusic", &fpsCombatMusic, true, "STR_FPS_COMBAT_MUSIC", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlares", &fpsFlares, true, "STR_FPS_FLARES", category));
	// Looks: the weapon in hand (always made of voxels), sprite upscaling (0 off, 1 smooth,
	// 2 smooth + detail), muzzle flashes, particles (0 none, 1 effects, 2 everything),
	// UFOs with steady hulls, glowing lights, coloured light and an aura, and the pixel size
	// (1 to 3: one ray colours a block of pixels that wide and high, for speed).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewModel", &fpsViewModel, true, "STR_FPS_VIEW_MODEL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteUpscale", &fpsSpriteUpscale, 2, "STR_FPS_SPRITE_UPSCALE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMuzzleFlash", &fpsMuzzleFlash, true, "STR_FPS_MUZZLE_FLASH", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsParticles", &fpsParticles, 2, "STR_FPS_PARTICLES", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUfoLights", &fpsUfoLights, true, "STR_FPS_UFO_LIGHTS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPixelSize", &fpsPixelSize, 0, "STR_FPS_PIXEL_SIZE", category));
	// The crosshair: shown or not, the shot info under it, and aiming at the exact point under
	// it (YES) or at the tile, as the original game does (NO).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshair", &fpsCrosshair, true, "STR_FPS_CROSSHAIR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairInfo", &fpsCrosshairInfo, true, "STR_FPS_CROSSHAIR_INFO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAimExact", &fpsAimExact, true, "STR_FPS_AIM_EXACT", category));
	// Staying in the 3D view during the alien turn, starting battles in it, and the debug cheat
	// "doom mode" (endless time units, energy and ammo).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurn3D", &fpsAlienTurn3D, true, "STR_FPS_ALIEN_TURN_3D", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStartInFirstPerson", &fpsStartInFirstPerson, false, "STR_FPS_START_IN_FIRST_PERSON", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomMode", &fpsDoomMode, false, "STR_FPS_DOOM_MODE", category));

	// The add-on's own keys, in the order the Controls list shows them, with their default keys.
	// Switching views, freeing the cursor from mouse look, and showing the classic HUD.
	options.push_back(OptionInfo(OPTION_OTHER, "keyBattleFirstPerson", &keyBattleFirstPerson, SDLK_F8, "STR_FPS_KEY_TOGGLE_VIEW", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsMouseMode", &keyFpsMouseMode, SDLK_c, "STR_FPS_KEY_MOUSE_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsClassicHud", &keyFpsClassicHud, SDLK_h, "STR_FPS_KEY_CLASSIC_HUD", category));
	// Walking and strafing (W, A, S, D).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsForward", &keyFpsForward, SDLK_w, "STR_FPS_KEY_FORWARD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsBack", &keyFpsBack, SDLK_s, "STR_FPS_KEY_BACK", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeLeft", &keyFpsStrafeLeft, SDLK_a, "STR_FPS_KEY_STRAFE_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeRight", &keyFpsStrafeRight, SDLK_d, "STR_FPS_KEY_STRAFE_RIGHT", category));
	// Fire modes (the next one, or snap, aimed and auto directly) and switching hands.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextMode", &keyFpsNextMode, SDLK_f, "STR_FPS_KEY_NEXT_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeSnap", &keyFpsModeSnap, SDLK_z, "STR_FPS_KEY_MODE_SNAP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAimed", &keyFpsModeAimed, SDLK_x, "STR_FPS_KEY_MODE_AIMED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAuto", &keyFpsModeAuto, SDLK_v, "STR_FPS_KEY_MODE_AUTO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsSwitchHand", &keyFpsSwitchHand, SDLK_g, "STR_FPS_KEY_SWITCH_HAND", category));
	// Opening doors (O, as E is the game's "use right hand"), flares, flight, jumping, night
	// vision, going up and down a level, and doom mode.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsOpenDoor", &keyFpsOpenDoor, SDLK_o, "STR_FPS_KEY_OPEN_DOOR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlare", &keyFpsFlare, SDLK_t, "STR_FPS_KEY_FLARE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlight", &keyFpsFlight, SDLK_b, "STR_FPS_KEY_FLIGHT", category));
	// Space is the map's night vision hold key; in 3D it jumps, and night vision is gear (N).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsJump", &keyFpsJump, SDLK_SPACE, "STR_FPS_KEY_JUMP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNightVision", &keyFpsNightVision, SDLK_n, "STR_FPS_KEY_NIGHT_VISION", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsUp", &keyFpsUp, SDLK_PAGEUP, "STR_FPS_KEY_UP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsDown", &keyFpsDown, SDLK_PAGEDOWN, "STR_FPS_KEY_DOWN", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsDoomMode", &keyFpsDoomMode, SDLK_INSERT, "STR_FPS_KEY_DOOM_MODE", category));
}

/**
 * Asks whether the options screen should offer the 3D VIEW tab: only when an active mod
 * switches the add-on on.
 * @param mod The game's loaded mods.
 * @return True to show the tab.
 */
bool hasOptionsTab(const Mod *mod)
{
	return mod->getFirstPerson().isEnabled();
}

/**
 * Creates the 3D VIEW options tab, when its tab button is pressed.
 * @param origin The part of the game the options were opened from (main menu, geoscape or
 *  battlescape).
 * @return The new tab's screen.
 */
State *createOptionsTab(OptionsOrigin origin)
{
	return new OptionsFirstPersonState(origin);
}

/**
 * Called by the game as an FPS-style shot fires each round: replaces the round's target with
 * where the first-person view aims it now (along the crosshair, with the spread, and the
 * recoil climbing round by round). Nothing changes while the 3D view isn't showing.
 * @param state The battlescape screen, whose first-person mode aims the shot.
 * @param action The shot.
 * @param target The point the round flies to, in voxels; changed in place.
 */
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target)
{
	FirstPersonMode::freeAim(state, action, target);
}

/**
 * Called by the game as a bullet hits: should the next round follow without the usual wait?
 * It should for the player's FPS-style shots (faithful targeting off) while the 3D view shows,
 * so a burst plays quickly.
 * @param state The battlescape screen.
 * @param attacker The unit that fired.
 * @return True if the hits should play quicker.
 */
bool fastShots(BattlescapeState *state, const BattleUnit *attacker)
{
	return FirstPersonMode::fastShots(state, attacker);
}

/**
 * Called by the game when a unit panics or goes berserk: the first-person view may show it its
 * own way (for the player's soldiers in the player's turn), and the game then leaves out its
 * little "... is panicking" infobox.
 * @param state The battlescape screen.
 * @param unit The unit breaking down.
 * @return True if the first-person view shows it.
 */
bool breakdown(BattlescapeState *state, BattleUnit *unit)
{
	return FirstPersonMode::breakdown(state, unit);
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
