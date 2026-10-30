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
#include <algorithm>
#include <map>
#include "FirstPersonMode.h"
#include "OptionsFirstPersonState.h"
#include "../Engine/Game.h"
#include "../Engine/OptionInfo.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleItem.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/**
 * Whether a battle with the mod's 3D view is on: its mode made (see create) and not yet
 * destroyed. For the rule fixes that come with the mod (see keepsFacingOnOwnTile), which units
 * ask without a way to the battle's screen.
 * @return The flag.
 */
bool &battleWithView()
{
	static bool on = false;
	return on;
}

}

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
	battleWithView() = true;
	return new FirstPersonMode(game, state);
}

/**
 * Destroys the first-person mode when its battlescape screen closes (nullptr is fine).
 * @param mode The mode, or nullptr.
 */
void destroy(FirstPersonMode *mode)
{
	if (mode)
	{
		battleWithView() = false;
	}
	delete mode;
}

/**
 * Does a unit keep its facing when it turns to a point on its own tile, straight above or below
 * it (BattleUnit::directionTo)? The game turned it south (atan2(0, -0)), so shooting at a wall it
 * stood against, or the floor at its feet, cost a turn first; in a battle with the mod's 3D view
 * it keeps its facing (and a turret its own). Without the mod, the game's own way.
 * @return True in a battle with the mod's 3D view.
 */
bool keepsFacingOnOwnTile()
{
	return battleWithView();
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
 * Settles settings that take their first value from an older one, once the options are loaded
 * (called when a battle or the 3D VIEW tab opens): the smoke (Original, Modern) from 0.9's
 * "See-through smoke": YES gives Modern, NO Original (1.0's Solid becomes Original too); the
 * flashlight (Off, On, Dynamic) from 1.0's "Flashlight" and "Tactical flashlight"; akimbo (Off,
 * Full, Realistic) from 1.08's "Full akimbo" and "Realistic akimbo" (and with akimbo Off, those
 * two, which now tell whether it's on just now, off: a hand-edited profile can't have two guns
 * fire with the setting Off, the akimbo key refusing to switch them off); and 1.0's next fire
 * mode key, when the player had moved it off F (F is the flashlight now), once.
 */
void settleOptions()
{
	Options::fpsSmoke = smokeMode();
	Options::fpsFlashlightMode = flashlightMode();
	Options::fpsAkimbo = akimboChoice();
	if (Options::fpsAkimbo == 0)
	{
		Options::fpsDualWield = Options::fpsAkimboRealistic = false;
	}
	if (Options::keyFpsNextMode != SDLK_f)
	{
		Options::keyFpsNextFireMode = Options::keyFpsNextMode;
		Options::keyFpsNextMode = SDLK_f;
	}
}

/**
 * The akimbo setting as it stands: Off (0), Full (1, any two guns) or Realistic (2, one-handed
 * guns only). Unset (-1: a profile from 1.08 not settled yet, or the defaults just restored on the
 * 3D VIEW tab), it's what 1.08's two settings say, which now only tell whether akimbo is on just
 * now (the akimbo key switches them, see FirstPersonMode::toggleAkimbo): "Realistic akimbo" YES
 * Realistic, else "Full akimbo" YES Full, NO Off.
 * @return 0, 1 or 2.
 */
int akimboChoice()
{
	if (Options::fpsAkimbo >= 0 && Options::fpsAkimbo <= 2)
	{
		return Options::fpsAkimbo;
	}
	return Options::fpsAkimboRealistic ? 2 : (Options::fpsDualWield ? 1 : 0);
}

/**
 * The flashlight setting as it stands: Off (0), On (1) or Dynamic (2). Unset (-1: a profile from
 * 1.0 not settled yet, or the defaults just restored on the 3D VIEW tab), it's what 1.0's two
 * settings say: "Tactical flashlight" YES Dynamic, else "Flashlight" YES On, NO Off.
 * @return 0, 1 or 2.
 */
int flashlightMode()
{
	if (Options::fpsFlashlightMode >= 0 && Options::fpsFlashlightMode <= 2)
	{
		return Options::fpsFlashlightMode;
	}
	return Options::fpsTacticalFlashlight ? 2 : (Options::fpsFlashlight ? 1 : 0);
}

/**
 * The smoke setting as it stands: Original (1) or Modern (2). 1.0's Solid (0) is gone: it's
 * Original now. Unset (-1: a profile from 0.9 not settled yet, or the defaults just restored on
 * the 3D VIEW tab), it's what 0.9's "See-through smoke" says: YES Modern, NO Original.
 * @return 1 or 2.
 */
int smokeMode()
{
	if (Options::fpsSmoke == 1 || Options::fpsSmoke == 2)
	{
		return Options::fpsSmoke;
	}
	if (Options::fpsSmoke == 0)
	{
		return 1;
	}
	return Options::fpsSeeThroughSmoke ? 2 : 1;
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
	// And on the isometric map, the selected soldier's helmet camera in a window: off, small,
	// medium or large (0 to 3).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMapCamera", &fpsMapCamera, 0, "STR_FPS_MAP_CAMERA", category));
	// Mouse look: how fast the view turns (1 to 20), up and down swapped or not, and the field
	// of view across the screen, in degrees (50 to 110).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMouseSensitivity", &fpsMouseSensitivity, 5, "STR_FPS_MOUSE_SENSITIVITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsInvertMouse", &fpsInvertMouse, false, "STR_FPS_INVERT_MOUSE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFieldOfView", &fpsFieldOfView, 75, "STR_FPS_FIELD_OF_VIEW", category));
	// Zoom: holding the zoom key (or the right button in mouse look) looks 4 times closer.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsZoom", &fpsZoom, true, "STR_FPS_ZOOM", category));
	// The middle button as on the isometric map: a click on a unit in sight shows it (the game's
	// unit screen); anywhere else it frees the cursor, as always.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMiddleClickIdentify", &fpsMiddleClickIdentify, true, "STR_FPS_MIDDLE_CLICK_IDENTIFY", category));
	// Turning: free, the soldier facing wherever you look (NO), or by the game's rules (YES): the
	// head looks up to 45 degrees off the soldier's facing for free, further turns them at the
	// game's cost.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTurnCost", &fpsTurnCost, false, "STR_FPS_TURN_COST", category));
	// The compass at the top right: where you look, the soldier's facing, and turning by the
	// rules, when looking further will cost time units.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCompass", &fpsCompass, true, "STR_FPS_COMPASS", category));
	// Moving: free walking or tile by tile (0 free, 1 tiles: a key press walks one tile), walking
	// speed in tiles per second (1 to 8; every tile still costs its time units),
	// the view bobbing with the steps, time units a flying soldier pays per level up or down
	// (0 = free) and whether that overrides the game's own cost for each unit, jumping over low
	// obstacles, falling off ledges, and how many voxels below the game's eye height the camera
	// sits (0 to 6).
	// (New installs start with the game's own mechanics where the 3D view adds its own: tile by
	// tile, flight at the game's cost, no vaulting; a player's report, GitHub issue #27.)
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMovement", &fpsMovement, 1, "STR_FPS_MOVEMENT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsWalkSpeed", &fpsWalkSpeed, 4, "STR_FPS_WALK_SPEED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHeadBob", &fpsHeadBob, true, "STR_FPS_HEAD_BOB", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCost", &fpsFlightCost, 0, "STR_FPS_FLIGHT_COST", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlightCostOverride", &fpsFlightCostOverride, false, "STR_FPS_FLIGHT_COST_OVERRIDE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsVault", &fpsVault, false, "STR_FPS_VAULT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFalling", &fpsFalling, true, "STR_FPS_FALLING", category));
	// A step off a ledge onto ground no one can walk on, or where no step leads out (a player's
	// report: X-Com Files' blood moat, which the map never lets a soldier into, soft-locked one),
	// stops at the edge with a warning; pushing that way again (or jumping) goes over.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSafeLanding", &fpsSafeLanding, true, "STR_FPS_SAFE_LANDING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEyeLower", &fpsEyeLower, 3, "STR_FPS_EYE_LOWER", category));
	// Shooting: the game's own accuracy (YES) or FPS-style shooting along the crosshair (NO; shown
	// the other way round, as "FPS-style shooting (easier)", NO at first: a player's report, GitHub
	// issue #27, new players switched it thinking it the faithful way), and firing a gun in each
	// hand at once (akimbo): off (0), any two guns (1, full), or one-handed guns only (2,
	// realistic). Unset (-1) until settled (see settleOptions) from 1.08's two settings, which
	// stay, unlisted, as whether akimbo is on just now (the akimbo key switches them; at most one).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFaithfulTargeting", &fpsFaithfulTargeting, true, "STR_FPS_FPS_STYLE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimbo", &fpsAkimbo, -1, "STR_FPS_AKIMBO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDualWield", &fpsDualWield, false, "STR_FPS_DUAL_WIELD", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimboRealistic", &fpsAkimboRealistic, false, "STR_FPS_AKIMBO_REALISTIC", "STR_FIRST_PERSON_SAVED"));
	// Akimbo's style: the two guns take turns round by round, each round resolved before the next
	// leaves, as an auto burst's (NO), or fire as a volley, both rounds at once and landing together
	// as a shotgun's pellets (YES); either way each less accurate, for the more expensive gun's shot
	// and a tenth. Its key switches it in play.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAkimboVolley", &fpsAkimboVolley, false, "STR_FPS_AKIMBO_VOLLEY", category));
	// Leaning: standing to one side of the tile's middle shoots from that side (the game's
	// off-centre origins), to peek round corners; and leaning out with the lean keys: off (0),
	// the eye moves to the side (1, peek), and the view tilts with it (2, tilt).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLeanShots", &fpsLeanShots, false, "STR_FPS_LEAN_SHOTS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsLean", &fpsLean, 0, "STR_FPS_LEAN", category));
	// Melee at the wall, door or object the soldier faces (the game's terrain melee, on even if
	// the mod leaves it off) and at a body lying in front.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMeleeTerrain", &fpsMeleeTerrain, false, "STR_FPS_MELEE_TERRAIN", category));
	// With terrain melee on (that setting or the mod's): melee at the floor or ceiling under the
	// crosshair too.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMeleeFloors", &fpsMeleeFloors, true, "STR_FPS_MELEE_FLOORS", category));
	// Light, atmosphere and sound: smooth lighting between tiles, ray-traced shadows, a
	// flashlight, night vision only with the right gear, tenser music while aliens are in sight,
	// the ticks of a hit and a kill, smoke drawn as the soldier's eyes see through it, and
	// unlimited tactical flares.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoothLighting", &fpsSmoothLighting, true, "STR_FPS_SMOOTH_LIGHTING", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsShadows", &fpsShadows, true, "STR_FPS_SHADOWS", category));
	// The distance haze on land: normal (0), light (1), clear (2: only a thin fade where the
	// view ends). Under the sea the murk stays.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHaze", &fpsHaze, 0, "STR_FPS_HAZE", category));
	// The flashlight, a light of the 3D view's own (on and off with its key, F; the suit light
	// stays the game's): none (0), a wide beam from the shoulder in the dark (1, on), or a light
	// on the weapon, a tight spot on the crosshair at any time of day (2, dynamic). Unset (-1)
	// until settled (see settleOptions) from 1.0's two settings, kept below, unlisted (a new name,
	// as 1.0's YES/NO wouldn't load into a number).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlightMode", &fpsFlashlightMode, -1, "STR_FPS_FLASHLIGHT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightVisionGear", &fpsNightVisionGear, false, "STR_FPS_NIGHT_VISION_GEAR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCombatMusic", &fpsCombatMusic, true, "STR_FPS_COMBAT_MUSIC", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitSounds", &fpsHitSounds, true, "STR_FPS_HIT_SOUNDS", category));
	// The smoke: the original games' own frames, see-through where the soldier sees through the
	// smoke and stacked where the sight runs out (1); soft clouds, solid where they block the
	// soldier's sight (2). (1.0's Solid, 0, is gone.) Unset (-1) until settled (see settleOptions)
	// from 0.9's "See-through smoke", kept below, unlisted.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmoke", &fpsSmoke, -1, "STR_FPS_SMOKE", category));
	// How opaque the see-through smoke is drawn (not where the soldier can't see through it:
	// that stays solid): the thinnest smoke's opacity in percent, thicker smoke more as before;
	// in the dark, by the light where it hangs, the night value (a player: "non full smoke can
	// be blinding" at night and on dark maps), the two blended in between.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSmokeOpacity", &fpsSmokeOpacity, 45, "STR_FPS_SMOKE_OPACITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsNightSmokeOpacity", &fpsNightSmokeOpacity, 25, "STR_FPS_NIGHT_SMOKE_OPACITY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlares", &fpsFlares, true, "STR_FPS_FLARES", category));
	// 0.9's "See-through smoke" (YES/NO), loaded and saved but listed nowhere (a category no
	// options screen shows): it gives the smoke setting its first value.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughSmoke", &fpsSeeThroughSmoke, true, "STR_FPS_SEE_THROUGH_SMOKE", "STR_FIRST_PERSON_SAVED"));
	// 1.0's "Flashlight" and "Tactical flashlight" (YES/NO), the same way: they give the
	// flashlight setting its first value.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFlashlight", &fpsFlashlight, true, "STR_FPS_FLASHLIGHT", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTacticalFlashlight", &fpsTacticalFlashlight, false, "STR_FPS_FLASHLIGHT", "STR_FIRST_PERSON_SAVED"));
	// Looks: the weapon in hand (always made of voxels), sprite upscaling (0 off, 1 smooth,
	// 2 smooth + detail), muzzle flashes, particles (0 none, 1 effects, 2 everything),
	// UFOs with steady hulls, glowing lights, coloured light and an aura, and the pixel size
	// (1 to 3: one ray colours a block of pixels that wide and high, for speed).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsViewModel", &fpsViewModel, true, "STR_FPS_VIEW_MODEL", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteUpscale", &fpsSpriteUpscale, 2, "STR_FPS_SPRITE_UPSCALE", category));
	// Soldiers' and aliens' sprites a little smaller (80 to 100% of the isometric map's size).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitSize", &fpsUnitSize, 100, "STR_FPS_UNIT_SIZE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsMuzzleFlash", &fpsMuzzleFlash, true, "STR_FPS_MUZZLE_FLASH", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsParticles", &fpsParticles, 2, "STR_FPS_PARTICLES", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUfoLights", &fpsUfoLights, true, "STR_FPS_UFO_LIGHTS", category));
	// Parts the map draws but gives no shape (no voxels: ladders, lifts' pads, walls of laser
	// beams), shown as see-through ghosts of their art: 0 none, 1 lifts and ladders (the parts
	// with the grav lift flag), 2 all of them.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostParts", &fpsGhostParts, 1, "STR_FPS_GHOST_PARTS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGhostOpacity", &fpsGhostOpacity, 50, "STR_FPS_GHOST_OPACITY", category));
	// A corpse or item pointed at, the game's line passing it to something behind: drawn faint, at
	// the ghost opacity (the game's shots pass through them).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSeeThroughCorpses", &fpsSeeThroughCorpses, false, "STR_FPS_SEE_THROUGH_CORPSES", category));
	// The map's edge as small red dots on the floor, nothing beyond it, instead of the tape.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDiscreetEdge", &fpsDiscreetEdge, false, "STR_FPS_DISCREET_EDGE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPixelSize", &fpsPixelSize, 0, "STR_FPS_PIXEL_SIZE", category));
	// The crosshair: shown or not, the shot info under it, aiming at the exact point under it
	// (YES) or at the tile, as the original game does (NO), and the units' hitboxes.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshair", &fpsCrosshair, true, "STR_FPS_CROSSHAIR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairInfo", &fpsCrosshairInfo, true, "STR_FPS_CROSSHAIR_INFO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsCrosshairCosts", &fpsCrosshairCosts, true, "STR_FPS_CROSSHAIR_COSTS", category));
	// A line under the crosshair as wide as a shot strays to either side: with the game's own
	// targeting, as far as a successful accuracy roll sends it at that range; in FPS style, the spread.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAccuracyLine", &fpsAccuracyLine, true, "STR_FPS_ACCURACY_LINE", category));
	// The name of the unit under the crosshair (or the free cursor), as the game names it.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsUnitNames", &fpsUnitNames, true, "STR_FPS_UNIT_NAMES", category));
	// How the stat bars read, on the map and in 3D (a player's report: a 300-HP unit's bar looked
	// full with a third of its health gone): the game's own (0: a pixel per point, cut off at the
	// bar's end), scaled (1: a stat shrinks for all of the player's units alike when a maximum is
	// longer than the bar, on the map and in 3D), or percentage (2: each bar full at its maximum).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStatBars", &fpsStatBars, 1, "STR_FPS_STAT_BARS", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAimExact", &fpsAimExact, true, "STR_FPS_AIM_EXACT", category));
	// The path a throw or shot would take, drawn while aiming: off (0), throws and arcing
	// shots (1), every shot too (2); and confirm fire: the first click marks the target and
	// shows its path, a second click on it fires.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsTrajectory", &fpsTrajectory, 1, "STR_FPS_TRAJECTORY", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsConfirmFire", &fpsConfirmFire, false, "STR_FPS_CONFIRM_FIRE", category));
	// An arcing shot the game would refuse (no curve of its own lands on the target tile, as at
	// a wall seen from the side) fires anyway, forced as with Ctrl held (its path drawn yellow).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsForceArcing", &fpsForceArcing, true, "STR_FPS_FORCE_ARCING", category));
	// A straight shot with the game's own targeting that finds no line of fire to the point under
	// the crosshair nor to any part of the target (off-centre too, with OXCE's option) fires anyway,
	// forced along the line to the crosshair, hitting what's in the way (its path drawn yellow).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsForceBlocked", &fpsForceBlocked, false, "STR_FPS_FORCE_BLOCKED", category));
	// A shotgun's pellets fly beside its round in the 3D view, each to where the game's spread
	// sends it, instead of all landing the moment it's fired (the game's own way, kept off).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsPelletsInFlight", &fpsPelletsInFlight, true, "STR_FPS_PELLETS_IN_FLIGHT", category));
	// A shotgun's pellets, and the two rounds of an akimbo volley, each land on their own: none is
	// lost when the lead pellet leaves the map, and in flight each hits as it arrives (a player's
	// report on OXCE's "shotgun void"); off, the game's own way, the lead pellet deciding.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsEachPellet", &fpsEachPellet, true, "STR_FPS_EACH_PELLET", category));
	// The units' hitboxes, outlined: off (0), fading in and out once a second (1), always (2), or
	// only the unit pointed at (3, the first for new installs: a player's report, GitHub issue #27,
	// new players aimed at a Sectoid's picture and missed its hitbox).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsHitboxes", &fpsHitboxes, 3, "STR_FPS_HITBOXES", category));
	// A unit hit where its picture shows (in a line of fire, a card facing the shooter; see
	// FirstPersonSpriteHits.cpp), for everyone's shots, in 3D and on the map, instead of the game's
	// voxel shapes (the owner's, GitHub issue #42). Off: the game's own.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsSpriteHitboxes", &fpsSpriteHitboxes, false, "STR_FPS_SPRITE_HITBOXES", category));
	// A unit killed or knocked out down at once for the battle, its fall playing on its own (twice
	// as fast, no turn to face the screen), several at once, in 3D and on the map; the soldier
	// looked through falls on their helmet camera card while the view goes on to the next (a
	// player's report, GitHub issue #40). Off: the game's own deaths, one by one.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsQuickDeaths", &fpsQuickDeaths, false, "STR_FPS_QUICK_DEATHS", category));
	// The hitboxes' outlines and the path of a throw glowing (spilling bloom round them), or plain lines.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsGlowingOutlines", &fpsGlowingOutlines, false, "STR_FPS_GLOWING_OUTLINES", category));
	// Icons over the downed (unconscious) units in sight, for burning, bleeding, shock and
	// stunned: off (0), over the body under the crosshair (1), over every body in sight (2).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDownedIcons", &fpsDownedIcons, 1, "STR_FPS_DOWNED_ICONS", category));
	// Staying in the 3D view during the alien turn, starting battles in it, and the debug cheat
	// "doom mode" (endless time units and energy), with its two extras, each doom mode plus
	// more: IDKFA (endless ammo) and IDDQD (invincible soldiers); in doom mode grenades go off in
	// real time (one click, a few seconds' fuse: the owner, "realtime grenade setting should be
	// always on with doom mode", where it was a setting of its own).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurn3D", &fpsAlienTurn3D, true, "STR_FPS_ALIEN_TURN_3D", category));
	// ...and in it, the view cuts to the soldier who sees the alien acting, or who fires back;
	// a soldier shot at, hit or taken over pops up their small camera.
	options.push_back(OptionInfo(OPTION_OTHER, "fpsAlienTurnCameras", &fpsAlienTurnCameras, true, "STR_FPS_ALIEN_TURN_CAMERAS", category));
	// ...and with them, a soldier of ours going down out of view pops up their camera (static).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsFallenSoldierCamera", &fpsFallenSoldierCamera, true, "STR_FPS_FALLEN_SOLDIER_CAMERA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsStartInFirstPerson", &fpsStartInFirstPerson, false, "STR_FPS_START_IN_FIRST_PERSON", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomMode", &fpsDoomMode, false, "STR_FPS_DOOM_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIdkfa", &fpsDoomIdkfa, false, "STR_FPS_DOOM_IDKFA", category));
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomIddqd", &fpsDoomIddqd, false, "STR_FPS_DOOM_IDDQD", category));
	// Doom mode's real-time grenades: armed as they're thrown, burning from leaving the hand (YES), or primed with a click and
	// burning in the hand (NO, "cooking");
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomPrimeOnThrow", &fpsDoomPrimeOnThrow, true, "STR_FPS_DOOM_PRIME_ON_THROW", category));
	// ...thrown freely at whatever the crosshair points at, bursting in the air when the fuse runs out in flight;
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomFreeGrenades", &fpsDoomFreeGrenades, false, "STR_FPS_DOOM_FREE_GRENADES", category));
	// ...and their fuse, in seconds (1 to 5; with prime on throw, FUSE in battle steps it too).
	options.push_back(OptionInfo(OPTION_OTHER, "fpsDoomGrenadeFuse", &fpsDoomGrenadeFuse, 3, "STR_FPS_DOOM_GRENADE_FUSE", category));

	// The add-on's own keys, in the order the Controls list shows them, with their default keys.
	// Switching views, freeing the cursor from mouse look, and showing the classic HUD.
	options.push_back(OptionInfo(OPTION_OTHER, "keyBattleFirstPerson", &keyBattleFirstPerson, SDLK_F8, "STR_FPS_KEY_TOGGLE_VIEW", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsMouseMode", &keyFpsMouseMode, SDLK_c, "STR_FPS_KEY_MOUSE_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsClassicHud", &keyFpsClassicHud, SDLK_h, "STR_FPS_KEY_CLASSIC_HUD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsHideHud", &keyFpsHideHud, SDLK_p, "STR_FPS_KEY_HIDE_HUD", category));
	// Walking and strafing (W, A, S, D).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsForward", &keyFpsForward, SDLK_w, "STR_FPS_KEY_FORWARD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsBack", &keyFpsBack, SDLK_s, "STR_FPS_KEY_BACK", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeLeft", &keyFpsStrafeLeft, SDLK_a, "STR_FPS_KEY_STRAFE_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsStrafeRight", &keyFpsStrafeRight, SDLK_d, "STR_FPS_KEY_STRAFE_RIGHT", category));
	// Two ways of walking, each on and off with its key: sprinting (the game's run; Caps Lock,
	// Doom's own "always run" key) and en passant (walking on past enemies coming into sight, as
	// Shift does on the map; the key left of 1, as Shift is the lean key).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsSprint", &keyFpsSprint, SDLK_CAPSLOCK, "STR_FPS_KEY_SPRINT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsEnPassant", &keyFpsEnPassant, SDLK_BACKQUOTE, "STR_FPS_KEY_EN_PASSANT", category));
	// Fire modes (the next one, or snap, aimed and auto directly) and switching hands. The next
	// fire mode is J (the game's "reserve TUs for kneel", which keeps it on the map), as F is the
	// flashlight. (1.0 saved it as keyFpsNextMode, on F: the new name leaves F behind, and a key
	// the player had picked instead carries over, see settleOptions.)
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextFireMode", &keyFpsNextFireMode, SDLK_j, "STR_FPS_KEY_NEXT_MODE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNextMode", &keyFpsNextMode, SDLK_f, "STR_FPS_KEY_NEXT_MODE", "STR_FIRST_PERSON_SAVED"));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeSnap", &keyFpsModeSnap, SDLK_z, "STR_FPS_KEY_MODE_SNAP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAimed", &keyFpsModeAimed, SDLK_x, "STR_FPS_KEY_MODE_AIMED", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsModeAuto", &keyFpsModeAuto, SDLK_v, "STR_FPS_KEY_MODE_AUTO", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsSwitchHand", &keyFpsSwitchHand, SDLK_g, "STR_FPS_KEY_SWITCH_HAND", category));
	// Akimbo on and off in battle (Y: one of the letters the battlescape leaves free).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsAkimbo", &keyFpsAkimbo, SDLK_y, "STR_FPS_KEY_AKIMBO", category));
	// Akimbo's style, both at once or in turn (setting "Akimbo in turn"): unbound unless the player binds one.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsAkimboStyle", &keyFpsAkimboStyle, SDLK_UNKNOWN, "STR_FPS_KEY_AKIMBO_STYLE", category));
	// Turning 45 degrees at a time, without stepping (comma and period: "<" and ">").
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsTurnLeft", &keyFpsTurnLeft, SDLK_COMMA, "STR_FPS_KEY_SNAP_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsTurnRight", &keyFpsTurnRight, SDLK_PERIOD, "STR_FPS_KEY_SNAP_RIGHT", category));
	// Leaning out (setting "Lean"): the lean key held with a strafe key (Left Shift and A or D),
	// so Q and E stay the game's hand keys; and a key of its own for each side, held, unbound
	// unless the player binds one. (0.9's own lean keys, Q and E, were saved as keyFpsLeanLeft
	// and keyFpsLeanRight: the new names leave those behind, so Q and E are the hands again.)
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanHold", &keyFpsLeanHold, SDLK_LSHIFT, "STR_FPS_KEY_LEAN_HOLD", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanLeftOwn", &keyFpsLeanLeftOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_LEFT", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsLeanRightOwn", &keyFpsLeanRightOwn, SDLK_UNKNOWN, "STR_FPS_KEY_LEAN_RIGHT", category));
	// Opening doors (O, as E is the game's "use right hand"), flares, flight, jumping, night
	// vision, going up and down a level, and doom mode.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsOpenDoor", &keyFpsOpenDoor, SDLK_o, "STR_FPS_KEY_OPEN_DOOR", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlare", &keyFpsFlare, SDLK_t, "STR_FPS_KEY_FLARE", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlight", &keyFpsFlight, SDLK_b, "STR_FPS_KEY_FLIGHT", category));
	// The flashlight on and off (F, as in the first-person shooters; setting "Flashlight").
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsFlashlight", &keyFpsFlashlight, SDLK_f, "STR_FPS_KEY_FLASHLIGHT", category));
	// Space is the map's night vision hold key; in 3D it jumps, and night vision is gear (N).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsJump", &keyFpsJump, SDLK_SPACE, "STR_FPS_KEY_JUMP", category));
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsNightVision", &keyFpsNightVision, SDLK_n, "STR_FPS_KEY_NIGHT_VISION", category));
	// Zoom, held (U: free in the game and the add-on).
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsZoom", &keyFpsZoom, SDLK_u, "STR_FPS_KEY_ZOOM", category));
	// Which way the units in sight face, held (Left Alt, as the map's own Alt shows its unit
	// indicators): a ring with an arrow on the floor round each, over everything.
	options.push_back(OptionInfo(OPTION_OTHER, "keyFpsShowFacing", &keyFpsShowFacing, SDLK_LALT, "STR_FPS_KEY_SHOW_FACING", category));
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
 * Asks whether a mod the game finds for the first time (one the player's mod list doesn't have
 * yet) starts switched on. Our own mod does, so a game built and laid out by hand plays in 3D
 * straight away, as a release (whose starting options list it on) does. The game asks only for
 * mods new to the list, so a player who switches it off keeps it off.
 * @param modId The new mod's id.
 * @return True to switch it on.
 */
bool activeWhenNew(const std::string &modId)
{
	return modId == "OpenXcom3D";
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
 * Called by the game as it works out where a shot leaves from (TileEngine::getOriginVoxel): an
 * FPS-style straight shot leaves from where the first-person view's eye is across the soldier's
 * tile, at the game's gun height (see FirstPersonMode::freeAimFrom). Nothing changes for any
 * other shot, or while the 3D view isn't showing.
 * @param state The battlescape screen.
 * @param action The shot.
 * @param origin The game's origin, in voxels; changed in place.
 */
void freeAimOrigin(BattlescapeState *state, const BattleAction &action, Position &origin)
{
	FirstPersonMode::freeAimFrom(state, action, origin);
}

/**
 * Called by the game when a close-quarters check knocks a shot aside: the 3D view looks where
 * the shot now goes (see FirstPersonMode::followDeflectedShot).
 * @param state The battlescape screen.
 * @param shooter The unit whose shot was knocked aside.
 * @param target The tile it now goes at.
 */
void closeQuartersDeflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target)
{
	FirstPersonMode::deflected(state, shooter, target);
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
 * Called by the game as it fires a shotgun's round: do its pellets fly beside it (setting
 * "Shotgun pellets in flight", while the 3D view shows), rather than all landing at once?
 * @param state The battlescape screen.
 * @return True if the pellets should fly.
 */
bool pelletsInFlight(BattlescapeState *state)
{
	return FirstPersonMode::pelletsInFlight(state);
}

/**
 * Called by the game as it fires a shotgun's round or an akimbo volley: does each pellet (and each
 * gun's round) land on its own (setting "Each pellet on its own"), rather than as the lead pellet
 * lands, the rest lost with it when it leaves the map?
 * @param state The battlescape screen.
 * @return True if each lands on its own.
 */
bool eachPellet(BattlescapeState *state)
{
	return FirstPersonMode::eachPellet(state);
}

/**
 * Called by the game as it fills the map's stat bars (the 3D view's are filled the same way): the scale to draw one at (see
 * FirstPersonMode::statBarScale).
 * @param state The battlescape screen.
 * @param stat Which stat (StatBar).
 * @param maximum The unit's maximum of it.
 * @param barWidth The bar's width, in pixels.
 * @return Pixels per point.
 */
double statBarScale(BattlescapeState *state, int stat, int maximum, int barWidth)
{
	return FirstPersonMode::statBarScale(state, stat, maximum, barWidth);
}

/**
 * Called by the game as a hand's weapon is clicked with Ctrl and Shift held: akimbo on the map
 * (see FirstPersonMode::toggleMapAkimbo).
 * @param state The battlescape screen.
 * @param unit The selected soldier.
 * @return True if it took the click (the game then does nothing else with it).
 */
bool toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit)
{
	return FirstPersonMode::toggleMapAkimbo(state, unit);
}

/**
 * Called by the game after it draws the hands' weapons: akimbo on the map's mini targets (see
 * FirstPersonMode::drawMapAkimbo).
 * @param state The battlescape screen.
 * @param unit The selected soldier (null: none the player can use).
 * @param leftHand The left hand's box.
 * @param rightHand The right hand's box.
 */
void drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand)
{
	FirstPersonMode::drawMapAkimbo(state, unit, leftHand, rightHand);
}

/**
 * Called by the game as it launches a shot clicked on the map: akimbo on the map (see
 * FirstPersonMode::mapAkimbo).
 * @param state The battlescape screen.
 * @param action The shot.
 */
void mapAkimbo(BattlescapeState *state, BattleAction &action)
{
	FirstPersonMode::mapAkimbo(state, action);
}

/**
 * A shotgun's pellet landed: heard, and marked in the 3D view (see FirstPersonMode::pelletLanded).
 * @param state The battlescape screen.
 * @param at Where it struck, in voxels.
 * @param from A point further back on its path, for its direction.
 * @param impact What it struck (a VoxelType; V_OUTOFBOUNDS: nothing).
 * @param weapon The weapon that fired it.
 * @param ammo Its ammo.
 * @param heard Whether it's heard (not one whose hit the explosion state brings, with its own sound).
 */
void pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard)
{
	FirstPersonMode::pelletLanded(state, at, from, impact, weapon, ammo, heard);
}

/**
 * The bullet sprite for shotgun ammo that has none (a player's report: on the isometric map a
 * shotgun's round is "unsprited"; "Just using the most common used sprite per that damage type
 * should be fitting for now"): of the mod's items with a bullet sprite, the one used most by those
 * of the ammo's damage type, or by all of them when none shares it. Worked out once per mod.
 * @param mod The mod.
 * @param ammo The shotgun ammo.
 * @return The bullet sprite, or Mod::NO_SURFACE when no item has one.
 */
int shotgunBulletSprite(const Mod *mod, const RuleItem *ammo)
{
	// The most used sprite per damage type (and, under DT_NONE's key -1, overall), per mod.
	static const Mod *countedFor = nullptr;
	static std::map<int, int> mostUsed;
	if (countedFor != mod)
	{
		countedFor = mod;
		mostUsed.clear();
		std::map<int, std::map<int, int>> votes; // damage type (-1: all) -> sprite -> items using it
		for (const std::string &name : mod->getItemsList())
		{
			const RuleItem *item = mod->getItem(name);
			if (item && item->getBulletSprite() != Mod::NO_SURFACE && item->getDamageType())
			{
				++votes[item->getDamageType()->ResistType][item->getBulletSprite()];
				++votes[-1][item->getBulletSprite()];
			}
		}
		for (const auto &[damage, sprites] : votes)
		{
			mostUsed[damage] = std::max_element(sprites.begin(), sprites.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
		}
	}
	auto found = mostUsed.find(ammo->getDamageType() ? (int)ammo->getDamageType()->ResistType : -1);
	if (found == mostUsed.end()) found = mostUsed.find(-1);
	return found != mostUsed.end() ? found->second : Mod::NO_SURFACE;
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

/**
 * Called by the game whenever a weapon would spend ammo: with Doom IDKFA on, the selected
 * soldier's shots in the 3D view spend none, so even a one-round clip (a rocket) isn't used up.
 * @param state The battlescape screen.
 * @param unit The unit holding the weapon.
 * @return True if the shot spends no ammo.
 */
bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit)
{
	return FirstPersonMode::endlessAmmo(state, unit);
}

/**
 * Called by the game whenever a unit is about to take damage: with Doom IDDQD on, X-COM's units
 * take none.
 * @param state The battlescape screen.
 * @param unit The unit about to be hurt.
 * @return True if it takes no damage.
 */
bool invincible(BattlescapeState *state, const BattleUnit *unit)
{
	return FirstPersonMode::invincible(state, unit);
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
