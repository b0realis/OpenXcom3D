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
 *  Date: 2026-11-06
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
 * FirstPersonXrShell.h: VR mode on the Meta Quest, as the game sees it (the Android build only):
 * one hook, in Screen::flip.
 *
 * The game plays on its pane (a Horizon OS panel) as ever. With the setting "VR mode" on, going
 * to the 3D view (F8) starts the immersive activity: the headset shows the 3D view in stereo,
 * the head looking round, the controllers playing; leaving the 3D view (F8, the B button) brings
 * the pane back. The game's thread never stops meanwhile. See FirstPersonXrShell.cpp for the
 * hand-over and the controls, FirstPersonXrSession.cpp for the OpenXR side.
 */
#include <SDL.h>

namespace OpenXcom
{

namespace FirstPerson
{

#ifdef __ANDROID__
/// Hands the frame just finished (the game's whole screen, 32-bit) to VR mode, and does VR's work
/// for the frame: true while the headset shows the game, and the pane isn't drawn.
bool xrShellFlip(SDL_Surface *screen);
/// Does VR mode hold the display now (the headset shows the game, or is taking over or handing
/// back)? Then the pane's window and SDL's renderer must be left alone: its panel has no surface
/// (Screen::resetDisplay waits, and is made good when the pane is back).
bool xrShellHoldsDisplay();
#endif

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
