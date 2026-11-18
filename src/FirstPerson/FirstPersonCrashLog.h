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
 * FirstPersonCrashLog.h: Android's crash log (the Meta Quest build only): a crash names its
 * place. OXCE had none there ("no stack trace information is available"): a crash's signal is
 * now caught with where it happened, the stack walked frame by frame, all of it written to the
 * game's log and the system's in the system crash report's frame format, then handed on to the
 * system's own handler. Two hooks: main (catchCrashSignals) and CrossPlatform::stackTrace
 * (logCrashPlace). See FirstPersonCrashLog.cpp, and quest3/BUILD.md for naming the source lines.
 */

namespace OpenXcom
{

namespace FirstPerson
{

#ifdef __ANDROID__
/// Catches the crash signals (a segfault, a bus error, an illegal instruction, a floating point
/// error, an abort) to log them with their place (from main, in place of OXCE's segfault handler).
void catchCrashSignals();
/// Logs the crash's place and the stack, frame by frame (from CrossPlatform::stackTrace).
/// @param context The registers where it happened (ucontext_t), or null: the crash being handled's, if any.
void logCrashPlace(void *context);
#endif

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
