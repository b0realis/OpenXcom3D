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
#include <climits>
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
const int ViewButtonX = 1, HudButtonX = 24, InventoryButtonX = 47, ToolButtonW = 22;
/// Where the squad buttons start, and the widths of the END and the previous / next buttons.
const int SquadX = 72;
const int EndTurnW = 30;
const int PrevNextW = 10;
/// Squad buttons shown at once (more scroll), and the least width of one.
const int SquadWindow = 10, SquadMinW = 13;
/// The info panel, bottom left, and the width of a line of text in it.
const int PanelW = 150, PanelH = 54, PanelLineW = PanelW - 6;
/// The crosshair's surface (square).
const int CrosshairSize = 41;
/// The soldier card under a hovered squad button, and its live helmet-camera view.
const int CardW = 124, CardY = 15, PreviewW = 116, PreviewH = 72;
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

/// How close (in voxels) the camera may get to a tile edge it can't cross.
const double EdgeMargin = 5.0;
/// How long the camera waits in front of a door it opened, in milliseconds (the 3D view swings
/// it open meanwhile).
const Uint32 DoorWait = 150;
/// How far from a closed door the camera stops (from the tile edge, in voxels).
const double DoorMargin = 11.0;
/// How long, in milliseconds, a ride up or down one level takes (a grav lift, or flying up and
/// down), and a fall or a landing.
const Uint32 RideTime = 1400, DropTime = 600;
/// A jump: a free hop this high (voxels) and this long (milliseconds); a vault over something
/// low arcs higher.
const double JumpHeight = 6.0, VaultHeight = 9.0;
const Uint32 JumpTime = 520;
/// The highest obstacle a soldier can jump over, in voxels: waist high (a soldier is about 22).
const int VaultMaxHeight = 14;

// ---- Fighting ----

/// A melee swing in the view: wind up, strike, back (milliseconds).
const Uint32 SwingTime = 420;
/// Doom mode (a first step towards a real-time mode): a grenade primed there burns for this
/// long (milliseconds) and goes off wherever it is, as in a first-person shooter.
const Uint32 DoomFuseTime = 3000;

// ---- What happens to the soldier (milliseconds) ----

/// How long the view stays with a soldier who was killed, knocked unconscious or taken over,
/// and how long the next soldier's view takes to come in out of it.
const Uint32 DeathCamTime = 2600, FaintTime = 2400, ControlledTime = 2400, AfterFallTime = 600;
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
