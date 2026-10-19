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

FirstPersonMode *create(Game *game, BattlescapeState *state)
{
	if (!game->getMod()->getFirstPerson().isEnabled())
	{
		return nullptr;
	}
	return new FirstPersonMode(game, state);
}

void destroy(FirstPersonMode *mode)
{
	delete mode;
}

void think(FirstPersonMode *mode)
{
	if (mode)
	{
		mode->think();
	}
}

bool handle(FirstPersonMode *mode, Action *action)
{
	return mode && mode->handle(action);
}

bool blit(FirstPersonMode *mode)
{
	return mode && mode->blit();
}

bool wantsMouseCapture(const FirstPersonMode *mode)
{
	return mode && mode->wantsMouseCapture();
}

void resized(FirstPersonMode *mode, int dX, int dY)
{
	if (mode)
	{
		mode->resized(dX, dY);
	}
}

/**
 * The add-on's options. Category STR_FIRST_PERSON is only listed by the 3D VIEW tab,
 * so they stay out of the other option screens.
 */
void createOptions(std::vector<OptionInfo> &info)
{
	using namespace Options;
	const char *cat = "STR_FIRST_PERSON";
	info.push_back(OptionInfo(OPTION_OTHER, "fpsSoldierPreview", &fpsSoldierPreview, true, "STR_FPS_SOLDIER_PREVIEW", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsMouseSensitivity", &fpsMouseSensitivity, 5, "STR_FPS_MOUSE_SENSITIVITY", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsInvertMouse", &fpsInvertMouse, false, "STR_FPS_INVERT_MOUSE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFieldOfView", &fpsFieldOfView, 75, "STR_FPS_FIELD_OF_VIEW", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsWalkSpeed", &fpsWalkSpeed, 4, "STR_FPS_WALK_SPEED", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsHeadBob", &fpsHeadBob, true, "STR_FPS_HEAD_BOB", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCost", &fpsFlightCost, 0, "STR_FPS_FLIGHT_COST", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsVault", &fpsVault, true, "STR_FPS_VAULT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFalling", &fpsFalling, true, "STR_FPS_FALLING", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsEyeLower", &fpsEyeLower, 3, "STR_FPS_EYE_LOWER", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFaithfulTargeting", &fpsFaithfulTargeting, true, "STR_FPS_FAITHFUL_TARGETING", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsDualWield", &fpsDualWield, false, "STR_FPS_DUAL_WIELD", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothLighting", &fpsSmoothLighting, true, "STR_FPS_SMOOTH_LIGHTING", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsShadows", &fpsShadows, true, "STR_FPS_SHADOWS", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlight", &fpsFlashlight, true, "STR_FPS_FLASHLIGHT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionGear", &fpsNightVisionGear, true, "STR_FPS_NIGHT_VISION_GEAR", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsCombatMusic", &fpsCombatMusic, true, "STR_FPS_COMBAT_MUSIC", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsFlares", &fpsFlares, true, "STR_FPS_FLARES", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsViewModel", &fpsViewModel, true, "STR_FPS_VIEW_MODEL", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsVoxelWeapon", &fpsVoxelWeapon, true, "STR_FPS_VOXEL_WEAPON", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteUpscale", &fpsSpriteUpscale, 2, "STR_FPS_SPRITE_UPSCALE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsMuzzleFlash", &fpsMuzzleFlash, true, "STR_FPS_MUZZLE_FLASH", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsParticles", &fpsParticles, 2, "STR_FPS_PARTICLES", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsPixelSize", &fpsPixelSize, 1, "STR_FPS_PIXEL_SIZE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshair", &fpsCrosshair, true, "STR_FPS_CROSSHAIR", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairInfo", &fpsCrosshairInfo, true, "STR_FPS_CROSSHAIR_INFO", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsAimExact", &fpsAimExact, true, "STR_FPS_AIM_EXACT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurn3D", &fpsAlienTurn3D, true, "STR_FPS_ALIEN_TURN_3D", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsStartInFirstPerson", &fpsStartInFirstPerson, false, "STR_FPS_START_IN_FIRST_PERSON", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "fpsDoomMode", &fpsDoomMode, false, "STR_FPS_DOOM_MODE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyBattleFirstPerson", &keyBattleFirstPerson, SDLK_F8, "STR_FPS_KEY_TOGGLE_VIEW", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsMouseMode", &keyFpsMouseMode, SDLK_c, "STR_FPS_KEY_MOUSE_MODE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsClassicHud", &keyFpsClassicHud, SDLK_h, "STR_FPS_KEY_CLASSIC_HUD", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsForward", &keyFpsForward, SDLK_w, "STR_FPS_KEY_FORWARD", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsBack", &keyFpsBack, SDLK_s, "STR_FPS_KEY_BACK", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeLeft", &keyFpsStrafeLeft, SDLK_a, "STR_FPS_KEY_STRAFE_LEFT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeRight", &keyFpsStrafeRight, SDLK_d, "STR_FPS_KEY_STRAFE_RIGHT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextMode", &keyFpsNextMode, SDLK_f, "STR_FPS_KEY_NEXT_MODE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeSnap", &keyFpsModeSnap, SDLK_z, "STR_FPS_KEY_MODE_SNAP", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAimed", &keyFpsModeAimed, SDLK_x, "STR_FPS_KEY_MODE_AIMED", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAuto", &keyFpsModeAuto, SDLK_v, "STR_FPS_KEY_MODE_AUTO", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsSwitchHand", &keyFpsSwitchHand, SDLK_g, "STR_FPS_KEY_SWITCH_HAND", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsOpenDoor", &keyFpsOpenDoor, SDLK_o, "STR_FPS_KEY_OPEN_DOOR", cat)); // E is the game's "use right hand"
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlare", &keyFpsFlare, SDLK_t, "STR_FPS_KEY_FLARE", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlight", &keyFpsFlight, SDLK_b, "STR_FPS_KEY_FLIGHT", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsJump", &keyFpsJump, SDLK_SPACE, "STR_FPS_KEY_JUMP", cat)); // the map's night vision hold key; in 3D night vision is gear (N)
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsNightVision", &keyFpsNightVision, SDLK_n, "STR_FPS_KEY_NIGHT_VISION", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsUp", &keyFpsUp, SDLK_PAGEUP, "STR_FPS_KEY_UP", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsDown", &keyFpsDown, SDLK_PAGEDOWN, "STR_FPS_KEY_DOWN", cat));
	info.push_back(OptionInfo(OPTION_OTHER, "keyFpsDoomMode", &keyFpsDoomMode, SDLK_INSERT, "STR_FPS_KEY_DOOM_MODE", cat));
}

bool hasOptionsTab(const Mod *mod)
{
	return mod->getFirstPerson().isEnabled();
}

State *createOptionsTab(OptionsOrigin origin)
{
	return new OptionsFirstPersonState(origin);
}

void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target)
{
	FirstPersonMode::freeAim(state, action, target);
}

bool fastShots(BattlescapeState *state, const BattleUnit *attacker)
{
	return FirstPersonMode::fastShots(state, attacker);
}

bool breakdown(BattlescapeState *state, BattleUnit *unit)
{
	return FirstPersonMode::breakdown(state, unit);
}

}

}
