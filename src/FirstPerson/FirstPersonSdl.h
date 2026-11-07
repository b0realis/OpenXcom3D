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
 * FirstPersonSdl.h: the few things SDL 1.2 and SDL2 spell differently, so that the add-on is one
 * code for both builds: the desktop ones (OXCE's SDL 1.2, through sdl12-compat on Linux) and the
 * Android one (upstream's `android` branch, on SDL2). These are the key codes and modifiers, the
 * keys held down, the keypad's keys, the lock keys and the mouse wheel. Everything else the
 * add-on uses is spelled the same in both.
 *
 * As the core's own SDL2Helpers.h does, the add-on is written with SDL2's names, and an SDL 1.2
 * build gets those names here.
 */
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2, 0, 0)
#include "../Engine/CrossPlatform.h"
#endif

#if !SDL_VERSION_ATLEAST(2, 0, 0)
/// A key code: SDL 1.2 calls the type SDLKey.
typedef SDLKey SDL_Keycode;
/// The modifier keys' bits: SDL 1.2 calls the type SDLMod.
typedef SDLMod SDL_Keymod;
#endif

namespace OpenXcom
{

// (Private to each file that includes this, as FirstPersonModeShared.h's helpers are.)
namespace
{

// The keypad's keys the 3D view walks and turns with (SDL2 spells them SDLK_KP_8, SDL 1.2
// SDLK_KP8), and the Num Lock key (SDL2: SDLK_NUMLOCKCLEAR).
#if SDL_VERSION_ATLEAST(2, 0, 0)
constexpr SDL_Keycode KeypadKey2 = SDLK_KP_2, KeypadKey4 = SDLK_KP_4, KeypadKey6 = SDLK_KP_6;
constexpr SDL_Keycode KeypadKey7 = SDLK_KP_7, KeypadKey8 = SDLK_KP_8, KeypadKey9 = SDLK_KP_9;
constexpr SDL_Keycode NumLockKey = SDLK_NUMLOCKCLEAR;
#else
constexpr SDL_Keycode KeypadKey2 = SDLK_KP2, KeypadKey4 = SDLK_KP4, KeypadKey6 = SDLK_KP6;
constexpr SDL_Keycode KeypadKey7 = SDLK_KP7, KeypadKey8 = SDLK_KP8, KeypadKey9 = SDLK_KP9;
constexpr SDL_Keycode NumLockKey = SDLK_NUMLOCK;
#endif

/// SDL 1.2 sends a lock key's (Caps Lock's, Num Lock's) "down" when its lock goes on and its "up"
/// when it goes off, one event for each press; SDL2 sends both for every press, as for any key.
#if SDL_VERSION_ATLEAST(2, 0, 0)
constexpr bool LockKeysSendOneEvent = false;
#else
constexpr bool LockKeysSendOneEvent = true;
#endif

/**
 * Is this key held down right now? SDL 1.2's key state is listed by key code, SDL2's by the
 * key's place on the keyboard (its scancode).
 * @param key The key (an unbound one, SDLK_UNKNOWN, is never held).
 * @return True while it's down.
 */
inline bool isKeyHeld(SDL_Keycode key)
{
	if (key == SDLK_UNKNOWN)
	{
		return false;
	}
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_GetKeyboardState(nullptr)[SDL_GetScancodeFromKey(key)] != 0;
#else
	return SDL_GetKeyState(nullptr)[key] != 0;
#endif
}

/**
 * The mouse wheel's step in an event: SDL 1.2 sends the wheel as the buttons "wheel up" and
 * "wheel down" (a press and a release for each notch), SDL2 as its own wheel event.
 * @param event The event.
 * @return -1 for a notch up (away from the player), 1 for a notch down, 0 for any other event.
 */
inline int wheelStep(const SDL_Event &event)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (event.type != SDL_MOUSEWHEEL || event.wheel.y == 0)
	{
		return 0;
	}
	// (A "natural" scrolling touchpad reports the wheel the other way round.)
	const int step = event.wheel.y > 0 ? -1 : 1;
	return event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -step : step;
#else
	if (event.type != SDL_MOUSEBUTTONDOWN)
	{
		return 0;
	}
	return event.button.button == SDL_BUTTON_WHEELUP ? -1 : event.button.button == SDL_BUTTON_WHEELDOWN ? 1 : 0;
#endif
}

/**
 * Sets an 8-bit surface's colours (SDL 1.2's SDL_SetColors; SDL2 sets them on the surface's
 * palette).
 * @param surface The surface.
 * @param colors The colours.
 * @param first The first palette index to set.
 * @param count How many.
 */
inline void setSurfaceColors(SDL_Surface *surface, const SDL_Color *colors, int first, int count)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetPaletteColors(surface->format->palette, colors, first, count);
#else
	SDL_SetColors(surface, const_cast<SDL_Color *>(colors), first, count);
#endif
}

/**
 * Is this event a mouse button's press or release, or a notch of the wheel (which SDL 1.2 sends
 * as button presses anyway)?
 * @param event The event.
 * @return True for those.
 */
inline bool isMouseButtonOrWheel(const SDL_Event &event)
{
	return event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP || wheelStep(event) != 0;
}

/**
 * Where the mouse is for a button or wheel event, in window pixels: a button event says where it
 * happened; SDL2's wheel event doesn't, so the pointer's position then (as the core takes it for
 * the wheel, a finger on a touch screen too).
 * @param event The button or wheel event.
 * @param x Set to the point's distance from the window's left edge.
 * @param y Set to its distance from the top.
 */
inline void mouseEventPosition(const SDL_Event &event, int &x, int &y)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (event.type == SDL_MOUSEWHEEL)
	{
		CrossPlatform::getPointerState(&x, &y);
		return;
	}
#endif
	x = event.button.x;
	y = event.button.y;
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
