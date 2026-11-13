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
 * FirstPersonXr.h: the inside of VR mode on the Meta Quest (Android only; FirstPersonXrShell.h
 * is its face to the game): what its two threads hand each other.
 *
 * - The game's thread (FirstPersonXrShell.cpp) plays the game as ever. In VR it draws the eyes,
 *   the HUD and, when another screen is over the battle, the game's whole screen (the
 *   "monitor"), each as a finished picture, and hands the newest of each over; and it turns the
 *   controllers into the game's keys and mouse.
 * - The OpenXR thread (FirstPersonXrSession.cpp) owns the OpenXR session. It runs at the
 *   headset's own rate (72 a second): it waits for each frame, reads the controllers and where
 *   the head and the eyes are, hands those over, uploads whatever new pictures came, and sends
 *   the frame's layers. A picture is shown again in every frame until a newer one comes, with
 *   the pose it was drawn for, so the headset turns it with the head between our (slower) frames.
 *
 * Neither thread waits for the other: each kind of thing goes through its own small box (Newest,
 * or a lock round a copy).
 */
#ifdef __ANDROID__
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <jni.h>
#include <SDL.h>
#include "FirstPersonVr.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace Xr
{

/// A picture for a layer, ready to upload: 32-bit RGBA (the bytes R, G, B, A in memory), the
/// bottom row first (as OpenGL's textures have it).
struct LayerPicture
{
	std::vector<Uint32> pixels;
	int width = 0, height = 0;
};

/**
 * The newest of something one thread makes and another takes (a "triple buffer"): the maker
 * fills back() and publishes it; the taker takes the newest published one into front() when a
 * new one came. Neither waits for the other longer than a swap of two numbers, and no picture is
 * copied.
 */
template <typename T>
class Newest
{
public:
	/// The maker's slot, to fill before publish().
	T &back()
	{
		return _slots[_back];
	}
	/// Hands the filled back slot over as the newest; the maker fills another one next.
	void publish()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		std::swap(_back, _ready);
		_fresh = true;
	}
	/// Takes the newest published slot into front(), if one came since the last take.
	bool take()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_fresh)
		{
			return false;
		}
		std::swap(_front, _ready);
		_fresh = false;
		return true;
	}
	/// The taker's slot: the one taken last.
	T &front()
	{
		return _slots[_front];
	}
	/// Which of the three slots the maker fills now (0 to 2): what it keeps outside the slot itself (the eyes' textures
	/// on the graphics card) goes by this number. The maker's thread only.
	int backIndex() const
	{
		return _back;
	}
	/// Forgets anything published but not taken (for a new session).
	void clear()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_fresh = false;
	}

private:
	std::mutex _mutex;
	T _slots[3];
	int _back = 0, _ready = 1, _front = 2;
	bool _fresh = false;
};

/// The depth the eyes' pictures are handed over with (XR_KHR_composition_layer_depth): the renderer's (see
/// vrProjectionDepth), 0 at the near plane and 1 at the far one (and beyond: the sky).
const float DepthNearMetres = VrDepthNearMetres, DepthFarMetres = VrDepthFarMetres;

/// Both eyes' pictures for the projection layer, with the views they were drawn for: each eye's
/// pose (in the session's LOCAL space) and the field of view its picture shows; and each eye's
/// depth (see DepthNearMetres; the bottom row first, as the pictures; empty without one).
/// Or, with the eyes kept on the graphics card (setting "VR eyes kept on the card", "Renderer": GPU): each eye's picture
/// and depth as textures of the renderer's context, which the OpenXR thread's context shares (the colour RGBA8 as a
/// layer's picture, the depth R32F, 0 to 1; the bottom row first), to read once the pair's fence has passed (an OpenGL
/// sync object). The pictures then hold only their sizes, but in the emulator test build's check, which hands over both.
struct EyePictures
{
	LayerPicture eyes[2];
	VrEyeView views[2];
	std::vector<float> depth[2];
	bool onCard = false;
	unsigned int cardColour[2] = {}, cardDepth[2] = {};
	void *cardFence = nullptr;
};

/// Where the OpenXR thread last found the head and the eyes (at a frame's predicted display
/// time), for the game's thread to draw with.
struct HeadSample
{
	bool valid = false;
	VrPose head = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
	VrEyeView views[2] = {};
	/// The controllers at the same time, in the same space (see HandPose): each pose, and whether it was tracked.
	VrPose hands[3] = {};
	bool handTracked[3] = {};
	uint64_t frame = 0;
};

/// The controllers' poses in a head sample (for the renderer's vrSetHands): the right hand's aim (its -z the way it
/// points) and grip (where the hand holds), and the left hand's aim.
enum HandPose
{
	HandRightAim, HandRightGrip, HandLeftAim, HandPoseCount
};

/// The controllers' buttons the game reads (see FirstPersonXrShell.cpp for what each does).
enum Button
{
	ButtonTriggerLeft, ButtonTriggerRight, ButtonGripLeft, ButtonGripRight,
	ButtonA, ButtonB, ButtonX, ButtonY, ButtonMenu, ButtonLeftStick, ButtonRightStick,
	ButtonCount
};

/// The controllers as the OpenXR thread last read them.
struct ControllerState
{
	float move[2] = {};                // the left stick: x right, y forward, each -1 to 1
	float turn[2] = {};                // the right stick, the same way
	bool held[ButtonCount] = {};       // each button, held now
	int presses[ButtonCount] = {};     // presses since the game's thread last took them (none lost however short)
	bool rayOnMonitor = false;         // the right controller's ray meets the monitor...
	float monitorX = 0.0f, monitorY = 0.0f; // ...there: 0 to 1 across from the left, 0 to 1 down from the top
	bool rayOnHud = false;             // the right controller's ray meets the HUD's panel...
	float hudX = 0.0f, hudY = 0.0f;    // ...there, the same way (the free cursor goes there: the top bar's buttons)
	bool focused = false;              // the session has the input (not under the system's menu)
};

/// Everything the two threads share.
struct Shared
{
	// From the game's thread to the OpenXR thread.
	Newest<EyePictures> eyes;              // the newest eye pictures
	Newest<LayerPicture> hud;              // the newest HUD picture
	Newest<LayerPicture> monitor;          // the newest picture of the game's whole screen
	std::atomic<bool> showHud{false};      // the HUD quad is wanted now
	std::atomic<bool> showMonitor{false};  // the monitor is wanted now
	std::atomic<bool> stopRequested{false};// leave VR: the session ends, the thread finishes
	std::atomic<bool> placeQuads{false};   // put the monitor and the HUD in front of the head again
	std::atomic<int> gameThreadId{0};      // the game's thread (Linux's id), for the runtime's scheduling
	std::atomic<bool> smoothing{true};     // the eyes smoothed by the graphics chip (setting "VR smoothing")
	std::atomic<bool> hudHeld{false};      // the cursor is free: the HUD holds still in the room (else fixed where the head looks)
	std::atomic<bool> cardRenders{false};    // setting "Renderer": GPU: the graphics chip draws the 3D world (the session asks for its high power level)
	std::atomic<bool> cardEyesWanted{false}; // setting "VR eyes kept on the card" with "Renderer": GPU: the next session's context joins the renderer's share group

	// From the OpenXR thread to the game's thread.
	std::mutex sampleMutex;
	HeadSample sample;                     // the newest head and eyes (under sampleMutex)
	std::mutex controllerMutex;
	ControllerState controllers;           // the controllers (under controllerMutex)
	std::atomic<bool> running{false};      // the session runs: frames go to the headset
	std::atomic<bool> finished{false};     // the thread is over (left, failed, or ended by the system)
	std::atomic<bool> failed{false};       // it couldn't start (no OpenXR runtime, no session...)
	std::atomic<int> recentres{0};         // the headset's recentrings so far (the game recentres for each)
	std::atomic<bool> cardEyes{false};     // its context shares the renderer's textures and can show eyes kept on the graphics card
};

/// The one shared block.
Shared &shared();

/// The context whose share group the OpenXR thread's context joins to read the renderer's textures (an EGLContext;
/// null: none). See FirstPersonGpu::shareContext.
void *rendererShareContext();

/// Starts the OpenXR thread for the immersive activity (from its onCreate, on Android's UI thread).
void startSession(JNIEnv *env, jobject activity);
/// Ends it: asks the session to end and waits for the thread, a few seconds at most (from the
/// activity's onDestroy, on the UI thread). Only the session of that activity: an older activity
/// going late (after a newer one started its own) leaves the newer one's alone.
void stopSession(JNIEnv *env, jobject activity);
/// Is the OpenXR thread still running?
bool sessionThreadAlive();

/// Writes a line to the system log (logcat tag OpenXcom3D), starting "VR ", and to the game's (see logToGame).
void log(int priority, const char *format, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 2, 3)))
#endif
	;
/// Hands a line to the game's log (openxcom.log), from any thread: the game's thread writes it at its next frame (the
/// game's log is written by one thread), so the owner's log keeps what the system's log may have lost.
void logToGame(const std::string &line);

}

}

}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
