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
 * FirstPersonModeShared.h: the layout, timings and small helpers that the FirstPersonMode*.cpp
 * files share. Only those files include it (everything here is private to each of them).
 */
#include <cctype>
#include <climits>
#include <string>
#include <SDL.h>
#include "../Mod/RuleItem.h"

namespace OpenXcom
{

namespace
{

// ---- Layout of the top bar, the info panel, the crosshair and the soldier card ----
// In canvas pixels: the battlescape's base resolution divided by the UI scale (see
// FirstPersonMode::uiScaleFor), so the layout is the same on every screen size.

/// The top bar: its row, its height, and where its tool buttons sit (3D, HUD, INV) and how wide.
const int BarY = 1, BarH = 12;
/// The compass (setting "Compass"): the disc's size, the room above it for the heading arrow, and
/// how far below the top bar it sits (canvas pixels).
const int CompassSize = 40, CompassArrowH = 7, CompassGap = 2;
/// The climb arrows beside the compass (see FirstPersonMode::updateClimbArrows): each ClimbArrowW x
/// ClimbArrowH pixels, the pair ClimbArrowsH high, ClimbArrowsGap pixels left of the dial.
const int ClimbArrowW = 13, ClimbArrowH = 9, ClimbArrowsH = 26, ClimbArrowsGap = 2;
/// Turning by the rules: the compass warns this many degrees before the view reaches the
/// head's limit (HeadTurn), where looking further turns the soldier.
const double CompassWarnDegrees = 10.0;
const int ViewButtonX = 1, HudButtonX = 24, InventoryButtonX = 47, ToolButtonW = 22;
/// Where the squad buttons start, and the widths of the END and the previous / next buttons.
const int SquadX = 72;
const int EndTurnW = 30;
const int PrevNextW = 10;
/// Squad buttons shown at once (more scroll; more on a screen with room for them at their full
/// width, see layoutTopBar), the least width of one and the full width.
const int SquadWindow = 10, SquadMinW = 13, SquadFullW = 18;
/// A shotgun's pellets landing are each heard (the owner: "all pellets should impact, play sound and
/// show bulletholes"), but at most this many within PelletSoundWindow ms: the mixer has 16 channels,
/// and two shotguns' sixteen pellets at once would take them all (see FirstPersonMode::pelletLanded).
const int PelletSoundsAtOnce = 4;
const Uint32 PelletSoundWindow = 60;
/// The info panel, bottom left, and the width of a line of text in it.
const int PanelW = 150, PanelH = 54, PanelLineW = PanelW - 6;
/// The crosshair's surface (square).
const int CrosshairSize = 41;
/// Where the game has pushed a soldier's sight under a floor right above (standing on something
/// tall under a ceiling), the 3D eye is only this many voxels under the game's (see eyeHeight).
const double PressedEyeLower = 1.0;
/// The soldier card under a hovered squad button, and its live helmet-camera view.
const int CardW = 124, CardY = 15, PreviewW = 116, PreviewH = 72;
/// The farthest the eye moves to get out of something solid (a big unit in a wide door, see clearEye), in voxels.
const double EyeClearReach = 10.0;
/// A melee blow at terrain by the crosshair (see FirstPersonMode::crosshairTerrainMelee) reaches this far from the eye, in voxels (a tile and a half).
const double MeleeTerrainReach = 24.0;
/// A soldier's rank beside their name (see FirstPersonMode::drawRankIcon): the icon's size in pixels,
/// the game's own battlescape badge (26x23) averaged down to it, or a mod's tiny rank (7x7) inside it;
/// and the room between the name and it.
const int RankIconW = 13, RankIconH = 11, RankIconGap = 3;
/// The badge's top is this many pixels above its text's top, so its bottom row is the letters' bottom
/// row (a player: it touched the weapon line under the name).
const int RankIconRise = 3;
/// The map camera's picture (setting "Map camera": small, medium, large; index 0 is off) takes
/// this share of the canvas's width (96, 128 or 160 pixels on the smallest canvas, 320 wide;
/// twice that on one 640 wide), in the card camera's proportions, and at most MapCameraMaxHeight
/// of the canvas's height; its frame adds MapCameraBorder round it and a title line
/// MapCameraTitleH high above it. It sits at the top left under the top bar (CardY), right of
/// OXCE's touch buttons when they show (MapCameraTouchClear screen pixels from the left: they
/// take x 2 to 33).
const float MapCameraShare[4] = { 0.0f, 0.30f, 0.40f, 0.50f }, MapCameraMaxHeight = 0.55f;
const int MapCameraBorder = 3, MapCameraTitleH = 10, MapCameraTouchClear = 36;
/// While an action is aimed on the map, the map camera looks this many voxels over the floor of
/// the tile under the cursor (with no unit seen there): about a crouching soldier's chest.
const double MapCameraAimHeight = 12.0;
/// The banner across the top of the view (big letters) and the line under it.
const int BannerW = 304;
/// The grav lift arrows.
const int LiftArrowW = 48, LiftArrowH = 14;

// ---- The weapon in hand ----

/// The weapon in hand is seen through the default 75 degree lens whatever the view's field of
/// view (1 / tan 37.5 = 1.303 times the 90 degree one), so it keeps its size and place.
const double WeaponLens = 1.303;
/// Where the weapon's muzzle (its bore) sits, for the right hand (the left mirrors it): right
/// of and below the crosshair, as a direction (x / depth, y / depth); through the lens that's
/// 0.42 half-widths of the view across and 0.45 down.
const double MuzzleScreen[2] = { 0.42 / WeaponLens, 0.45 / WeaponLens };
/// How much of the line from the stock point to the muzzle the weapon in hand fills.
const double WeaponScale = 0.9;
/// A weapon picture whose long axis is within this many degrees of upright (or of level) is
/// taken as exactly upright (or level): the games draw their weapons standing straight up, and a
/// grip, a magazine or a bulky body only tips the measured axis off the barrel.
const double WeaponUprightSnap = 30.0;
/// ...if the picture lies no more than this many times as wide across the upright (or level) axis as
/// across its measured one (and a pixel): a pole drawn on a slant keeps its slant.
const double WeaponSnapSpread = 1.25;
/// The way a melee weapon's tip points at rest, from its grip (camera space: x out to the weapon's side, y down, z ahead; any length).
const double MeleeUpright[3] = { 0.3, -1.0, 0.25 };
/// The aliens' turn cameras (setting "Aliens' turn cameras"): the least time the view stays with a soldier
/// before cutting to another who sees the alien acting, and how long a popped-up camera shows (ms).
const Uint32 AlienCameraHold = 1500, AlertTime = 3000;
/// Zoom (setting "Zoom"): how much closer the view looks, and how many seconds it takes to glide
/// all the way in (or out), so the SCOPING line's number can be read as it climbs.
const double ZoomFactor = 4.0, ZoomSeconds = 0.7;
/// The scope cross shown while zoomed: its gap round the reticle and the spacing of its ticks, in canvas pixels.
const int ScopeGap = 12, ScopeTickSpacing = 24;
/// Looking down past LookDownSlideFrom degrees, the weapon in hand slides out of sight, gone at LookDownSlideTo.
const double LookDownSlideFrom = 45.0, LookDownSlideTo = 72.0;
/// A held item that isn't a weapon (grenade, medi-kit, scanner, clip...) is shown as voxels in
/// the bottom corner, its lower half out of sight: 2.6 canvas pixels per picture pixel on a
/// 200-line canvas (in proportion on bigger ones), a smaller picture than HeldItemMinPixels
/// across shown as big as that, and turned so its depth shows (degrees, for the right hand; the
/// left mirrors it): towards the screen's centre, its top tipped back, leaning in.
const double HeldItemScale = 2.6, HeldItemMinPixels = 20.0, HeldItemYaw = 22.0, HeldItemPitch = -14.0, HeldItemRoll = -10.0;
/// A weapon shorter than the rifle (whose picture is about RiflePixels long) comes out smaller:
/// by its length to the power SizeCurve (a pistol, half a rifle's picture, at 70%), shrunk about
/// a point GripAt of the way from the muzzle towards the stock end (so it still points at the
/// crosshair).
const double RiflePixels = 41.0;
const double SizeCurve = 0.6;
const double GripAt = 0.3;

// ---- Moving ----

/// Turning by the rules (setting "Turning costs TU"): how far, in degrees, the view may look off
/// the soldier's facing before they turn (their head turns that far for free).
const double HeadTurn = 45.0;
/// The classic HUD's map level buttons in 3D: a press tilts the view this many degrees up (or
/// down), towards the level above (or below), at SnapTurnSpeed.
const double LevelLookDegrees = 30.0;
/// How fast a snap turn (the turn keys) or a level look turns the view, in degrees a second.
const double SnapTurnSpeed = 360.0;
/// Leaning (setting "Lean shots"): how far, in voxels, the camera must stand to one side of its
/// tile's middle, across the line of fire, for a shot to start from that side.
const double LeanThreshold = 2.0;
/// Leaning out (setting "Lean", the lean keys held): how far the eye goes to the side at most,
/// in voxels; how much lower it gets there (a head bending sideways); how long a lean takes to
/// go all the way out (or back in), in seconds; how far short of anything solid the eye stops,
/// and the steps it is looked for in, in voxels; and how far the view tilts with "Tilt", in
/// degrees.
const double LeanReach = 6.0;
const double LeanDrop = 1.5;
const double LeanTime = 0.2;
const double LeanWallMargin = 2.0;
const double LeanCheckStep = 0.5;
const double LeanTiltDegrees = 10.0;

/// How close (in voxels) the camera may get to a tile edge it can't cross.
const double EdgeMargin = 5.0;
/// Tile moves: how long the movement keys are held before the move starts, in milliseconds (a
/// second key pressed with the first, W and D say, makes it a diagonal).
const Uint32 TileMoveKeyWait = 60;
/// The numpad: a move key's press waits this long (ms) for a key of the other axis to join it (8
/// and 4 together: a diagonal, as W and A make one), so pressing two at once isn't two moves.
const Uint32 NumpadComboWait = 60;
/// Tile moves: a move that hasn't come closer to its tile for this long (ms) is over (the step
/// was refused, or a door or a vault moved the camera on its own).
const Uint32 TileMoveStuck = 300;
/// A numpad move's speed, voxels a second: a tile in a tenth of a second, almost at once, like
/// a step on the isometric map.
const double NumpadMoveSpeed = 160.0;
/// Sprinting (its key): the walking pace times this, while the soldier runs (the game's run).
const double SprintPace = 1.6;
/// On a grav lift or a ladder, W climbs while the view looks up more than this many degrees,
/// and goes down while it looks down as much (S the other way round); see updateClimb.
const double ClimbPitch = 30.0;
/// A climb with W the game refused (not enough time units, say) is tried again after this many
/// milliseconds while the key stays held, so its message isn't repeated every frame.
const Uint32 ClimbRetryTime = 700;
/// How long the camera waits in front of a door it opened, in milliseconds (the 3D view swings
/// it open meanwhile).
const Uint32 DoorWait = 150;
/// Shots and explosions in 3D: while they're on the map, the battle's timer may run up to
/// CatchUpSteps steps it missed during a frame, so a burst fires and a long chain goes off at the
/// isometric map's pace (a fast hit's or a chain's steps come a millisecond apart, and the map
/// runs them between its quick frames: about a 50 ms frame's worth); and for CatchUpGrace
/// milliseconds after, for the step that fires a burst's next round (see catchUp).
const int CatchUpSteps = 50;
const Uint32 CatchUpGrace = 100;
/// The open door key, with no door under the crosshair: it opens the closed door within reach
/// nearest the way the view looks, up to this many degrees off it (see doorWithinReach).
const double DoorPointSlack = 60.0;
/// Where the view tips to when a failed close-quarters check knocks the shot at the soldier's
/// feet or straight up (degrees of pitch; see followDeflectedShot).
const double DeflectedDownPitch = -75.0, DeflectedUpPitch = 60.0;
/// How far from a closed door the camera stops (from the tile edge, in voxels).
const double DoorMargin = 11.0;
/// Up or down one level (a grav lift, a ladder, flying, a fall) takes as long as walking RideTiles
/// tiles at RidePace of the walk speed setting (a level is 24 voxels high, a tile 16 across; see
/// rideTime): 0.75 s at the usual 4 tiles a second (a fall speeding up within it, as by gravity).
/// (All took 1.4 s at any speed, while the game itself is done in about 0.3 s: a player's report on
/// 1.03.) Any other change of level where the soldier stands glides in DropTime milliseconds.
const double RideTiles = 1.5, RidePace = 0.5;
const Uint32 DropTime = 600;
/// On landing after a fall the view dips LandingDip voxels and comes back up, over LandingDipTime
/// milliseconds.
const double LandingDip = 2.0;
const Uint32 LandingDipTime = 250;
/// A jump: a free hop this high (voxels) and this long (milliseconds); a vault over something
/// low arcs higher.
const double JumpHeight = 6.0, VaultHeight = 9.0;
const Uint32 JumpTime = 520;
/// The highest obstacle a soldier can jump over, in voxels: waist high (a soldier is about 22).
const int VaultMaxHeight = 14;

// ---- Fighting ----

/// A melee swing in the view: wind up, strike, back (milliseconds).
const Uint32 SwingTime = 420;
/// Doom free grenades: their gravity (voxels a second, each second, down), the usual speed of a throw (voxels a
/// second: the flight time is the distance over it, within FreeGrenadeTimeMin and Max seconds).
const float FreeGrenadeGravity = 360.0f, FreeGrenadeSpeed = 240.0f, FreeGrenadeTimeMin = 0.25f, FreeGrenadeTimeMax = 1.4f;
/// Doom free grenades reach no further than a person can throw (the game's own reach is far longer: 21 tiles at strength 27, 51
/// at 60): FreeGrenadeMaxTiles (25 tiles, about 40 m, taking a tile as about 1.6 m from a soldier's height) at strength
/// FreeGrenadeFullStrength or more, less in proportion below it.
const float FreeGrenadeMaxTiles = 25.0f, FreeGrenadeFullStrength = 60.0f;
/// Doom mode with real-time grenades (a first step towards a real-time mode): a thrown grenade
/// burns for the setting "Doom grenade fuse" (1 to FirstPerson::DoomFuseMaxSeconds seconds) from
/// the moment it leaves the hand, then goes off wherever it is, as in a first-person shooter.

// ---- What happens to the soldier (milliseconds) ----

/// How long the view stays with a soldier who was killed, knocked unconscious or taken over,
/// and how long the next soldier's view takes to come in out of it.
const Uint32 DeathCamTime = 2600, FaintTime = 2400, ControlledTime = 2400, AfterFallTime = 600;
/// Looking through a unit under our mind control: a purple haze at the view's edges (see
/// controlHaze), ControlHazeStrength strong, breathing in and out (by ControlHazeBreath of it) every
/// ControlHazePeriod milliseconds.
const float ControlHazeStrength = 0.7f, ControlHazeBreath = 0.25f;
const Uint32 ControlHazePeriod = 2400;
/// A soldier killed and turned into a zombie: down, up again, and the camera gives out.
const Uint32 ZombieCamTime = 4200;
/// A soldier's breakdown (panic, berserk): the game holds this long while the view washes out
/// (as long as its own "has panicked" box would stay up), the screen shows at least so long,
/// and coming to (the white fading away) takes this.
const Uint32 BreakdownPauseTime = 2000, BreakdownMinTime = 2600, ComeToTime = 1200;

const double Pi = 3.14159265358979323846;

// ---- Colours for our texts and drawings ----
// The game draws in a 256-colour palette; these find the palette entries closest to a colour.
// Distances weigh green most and blue least, roughly as the eye does.

/**
 * The palette colour nearest to (red, green, blue). Index 0 is transparent and never picked.
 */
inline Uint8 nearestColor(const SDL_Color *palette, int red, int green, int blue)
{
	int best = 1, bestDistance = INT_MAX;
	for (int index = 1; index < 256; ++index)
	{
		const int dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
		const int distance = dr * dr * 3 + dg * dg * 6 + db * db;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = index;
		}
	}
	return (Uint8)best;
}

/**
 * The start of the colour group whose text shades look most like (red, green, blue). The game's
 * Text draws glyphs in its colour + 3, 6 and 9 (high contrast), which must stay inside one
 * 16-colour group, so a text colour is always a group's start.
 */
inline Uint8 textColor(const SDL_Color *palette, int red, int green, int blue)
{
	int best = 16, bestDistance = INT_MAX;
	for (int group = 0; group < 16; ++group)
	{
		const SDL_Color &glyphShade = palette[group * 16 + 3];
		const int dr = glyphShade.r - red, dg = glyphShade.g - green, db = glyphShade.b - blue;
		const int distance = dr * dr * 3 + dg * dg * 6 + db * db;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = group * 16;
		}
	}
	return (Uint8)best;
}

/**
 * A text colour for letters of any shade, dark ones too (with high contrast off): the palette
 * index whose glyph shades (the next five) come nearest to (red, green, blue), inside one
 * colour group.
 */
inline Uint8 inkColor(const SDL_Color *palette, int red, int green, int blue)
{
	int best = 0, bestDistance = INT_MAX;
	for (int index = 0; index < 256; ++index)
	{
		// Its glyph shades must not run into the next group.
		if ((index & 15) > 10) continue;
		const SDL_Color &glyphShade = palette[index + 3];
		const int dr = glyphShade.r - red, dg = glyphShade.g - green, db = glyphShade.b - blue;
		const int distance = dr * dr * 3 + dg * dg * 6 + db * db;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = index;
		}
	}
	return (Uint8)best;
}

/**
 * A key's name in capitals, as the game spells key names (e.g. "PAGE UP"), for messages that
 * tell the player which key to press.
 */
inline std::string keyLabel(SDLKey key)
{
	// An unbound key has no name to show (SDL would say "UNKNOWN KEY").
	if (key == SDLK_UNKNOWN)
	{
		return "";
	}
	std::string text = SDL_GetKeyName(key);
	for (char &letter : text) letter = (char)toupper((unsigned char)letter);
	return text;
}

/**
 * Is this action a gun's shot (snap, aimed or auto), as opposed to a throw, a melee hit, psi or
 * using an item?
 */
inline bool isShot(BattleActionType mode)
{
	return mode == BA_SNAPSHOT || mode == BA_AIMEDSHOT || mode == BA_AUTOSHOT;
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
