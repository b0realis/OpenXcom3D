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
 * FirstPersonXrShell.cpp: VR mode on the Meta Quest, the game's side (the Android build only;
 * the OpenXR side is FirstPersonXrSession.cpp, the two meet in FirstPersonXr.h).
 *
 * The app is a "hybrid": the game is a panel (SDL's activity, on the pane, as the owner plays
 * it), and VR is an immersive activity of the same app (VrActivity) that only lives while the
 * 3D view is in the headset. The game's thread runs on throughout, so the battle never stops:
 *
 *   pane      VR mode on and the 3D view on (F8): SDL is told not to stop the game when the
 *             panel goes to the background, the panel's going-away events are kept from the
 *             game (it would pause itself and its sound), and the panel starts VrActivity.
 *   entering  the frames go to the headset, not the pane, until the OpenXR session runs.
 *   immersive every frame: the newest head pose to the 3D view, both eyes drawn and handed
 *             over (only while the battle is the screen on top: under the inventory, a message
 *             or the menu the last eyes stay, frozen round the player, and the game's whole
 *             screen shows on a "monitor" in front, the right controller's ray its mouse), the
 *             HUD handed over, the controllers turned into the game's own keys and mouse.
 *   leaving   the 3D view was left (vrInThreeD: F8, the B button, the battle's end; and the
 *             aliens' turn, which VR mode plays on the pane, VR coming back by itself on the
 *             player's next turn): the session ends,
 *             VrActivity asks Horizon OS for the panel (its documented way back: Home, with the
 *             panel to open there) and goes; once SDL has the panel again (resumed, its surface
 *             ready, its context back), the pane, the display's reset in its first frame.
 *
 * The controls in VR (the owner's table in INSTALL_QUEST.md):
 *   in 3D       the right controller = the weapon (aim, laser, crosshair; its poses, the left
 *               hand's aim and the left grip go to the renderer: vrSetHands); left stick = walk
 *               and strafe along the soldier's way (the movement keys, held while the stick is
 *               pushed half-way); right stick left/right = turn him (vrSetTurnAxis: smooth or
 *               snap, the setting "VR turning"), up/down = the up and down keys; right trigger =
 *               the left mouse button (fire where the weapon points; with the cursor free, a click); left trigger = mouse look / free
 *               cursor (the add-on's key, C on the PC; the owner's choice), once a pull: with the
 *               cursor free, the right controller's ray moves it over the HUD's panel (its top
 *               bar's buttons; the cursor drawn there, see drawHudCursor); right grip = the right
 *               mouse button; A = the inventory; B = back to the pane (the 3D view's key, F8);
 *               X = kneel; Y held a second = end the turn; left stick click = sprint; right stick
 *               click = recentre; left menu button = Esc.
 *   on the      the right controller's ray = the mouse; the right trigger = the left button; the
 *   monitor     right grip = the right button; right stick up/down = the wheel; A = the
 *               inventory key (it closes the inventory); B and the menu button = Esc; the left
 *               trigger nothing (the cursor is free there anyway; C would be the inventory's
 *               template key).
 *
 * Everything is logged for the headset's tests (tag OpenXcom3D, lines starting "VR"; the
 * controllers' first use under OpenXcom3DInput), with every few seconds the eyes' drawing time;
 * all of it in the game's log too (openxcom.log; other threads' lines through Xr::logToGame).
 */
#ifdef __ANDROID__
#include "FirstPersonXrShell.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <sstream>
#include <android/log.h>
#include <jni.h>
#include <unistd.h>
#include <SDL_system.h>
#include "FirstPersonXr.h"
#include "FirstPersonGpu.h"
#include "FirstPersonVr.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/State.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"

// SDL 2.0.9's own input entry points (src/events/SDL_keyboard_c.h, SDL_mouse_c.h; SDL is built
// into the game's library): the calls its Android keyboard and mouse code make, so the game gets
// exactly a keyboard's and a mouse's events from the controllers, and the key state the 3D view
// reads for walking (isKeyHeld).
extern "C"
{
int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);
int SDL_SendMouseButton(SDL_Window *window, Uint32 mouseID, Uint8 state, Uint8 button);
int SDL_SendMouseMotion(SDL_Window *window, Uint32 mouseID, int relative, int x, int y);
int SDL_SendMouseWheel(SDL_Window *window, Uint32 mouseID, float x, float y, SDL_MouseWheelDirection direction);
}

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// The 3D view counts as left only after vrInThreeD has said so this long (ms): a switch of the headset takes seconds,
/// so a moment's flicker mustn't cause one (vrInThreeD itself holds through 2D screens and untracked moments).
const Uint32 LeaveGraceMs = 250;
/// The immersive activity's session must be running this soon after it was asked for (ms), else VR is given up (for
/// this time in 3D) and the pane stays.
const Uint32 EnterTimeoutMs = 10000;
/// Leaving, the panel is asked for once the session has ended, or after this long anyway (ms).
const Uint32 PanelAskMs = 2500;
/// ...and the pane is drawn again once the panel is back, or after this long anyway (ms).
const Uint32 PanelWaitMs = 10000;
/// The sticks: pushed this far (0 to 1) they walk (held), this far they flick (one turn, one step up or down, a
/// notch of the wheel), and back within this they are ready to flick again.
const float StickWalk = 0.5f, StickFlick = 0.6f, StickRearm = 0.3f;
/// Y held this long (ms) ends the turn (a short press does nothing: no turn ended by a slip).
const Uint32 EndTurnHoldMs = 1000;
/// The wheel from the right stick on the monitor: a notch, then another every this long while it's held (ms).
const Uint32 WheelRepeatMs = 150;
/// The monitor's picture is the game's screen at twice its size while that stays within this (pixels a side; the
/// headset's swapchains take 4096 and more, the game's usual 640x400 makes 1280x800).
const int MonitorMaxSize = 2048;
/// The probe's reports every this many seconds (as the 3D view's own frame probe).
const int ProbeSeconds = 5;
/// SDL's hints our patched SDL reads (see the wrapper's jni/sdl_patches/SDL_androidevents.c): block the game while
/// its panel is in the background, and pause the sound then.
const char *const HintBlockOnPause = "SDL_ANDROID_BLOCK_ON_PAUSE";
const char *const HintPauseAudio = "SDL_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO";

/// Where VR mode is (see the file's comment).
enum class ShellState
{
	Pane, Entering, Immersive, Leaving
};

/// What the controllers hold down in the game now, and the sticks' flick state.
struct ControllerInput
{
	bool keyHeld[SDL_NUM_SCANCODES] = {};   // keys held down for the controllers
	bool buttonHeld[8] = {};                // mouse buttons held (SDL_BUTTON_LEFT...)
	bool upDownReady = true;                // the right stick back near the middle since its last flick up or down
	Uint32 yHeldSince = 0;                  // Y held since then (0: not held)
	bool endTurnSent = false;               // ...and the turn already ended for this hold
	Uint32 wheelNextAt = 0;                 // the next notch of the wheel while the stick is held
	int mouseX = -1, mouseY = -1;           // where the ray last put the mouse (window pixels)
	bool logged[Xr::ButtonCount] = {};      // each button's first use logged
	bool monitorLogged = false;             // the ray's first use on the monitor logged
	bool hudRayLogged = false;              // ...and on the HUD's panel (the free cursor)
	int cursorFreeWas = -1;                 // the cursor was free last frame (1) or mouse look on (0); -1 unknown
	int monitorWas = -1;                    // the monitor was up last frame (1) or the battle on top (0); -1 unknown
};

/// The game thread's side of VR mode.
struct Shell
{
	ShellState state = ShellState::Pane;
	Uint32 stateSince = 0;           // when it came to this state
	bool declined = false;           // VR given up for this time in 3D (it failed, or the system ended it)
	Uint32 threeDLostAt = 0;         // when the 3D view went (0: it's on)
	bool panelAsked = false;         // leaving: the panel was asked for
	Uint32 frames = 0;               // the game's frames so far (in this hook)
	Uint32 panelBackFrame = 0;       // leaving: the frame in which the panel was seen back (0: not yet)
	bool displayResetDue = false;    // back on the pane: the display's reset waits for the pane's first frame (see backOnPane)
	int recentresSeen = 0;           // the headset's recentrings the game has followed
	bool filterInstalled = false;    // the event filter is in (once)
	ControllerInput input;
	// The probe (see reportProbe).
	Uint32 probeSince = 0;
	int probePairs = 0, probeMonitors = 0, probeHuds = 0, probeFrames = 0;
	double probeTotalMs = 0.0, probeWorstMs = 0.0, probeHandOverMs = 0.0; // (the last: making the pair ready for the OpenXR thread)
	int eyesOnCard = -1;               // the last pair was kept on the graphics card (1) or handed over as pictures (0); -1: none yet (logged when it changes)
};
Shell shell;

/// While the pane hands over to VR and back, its going-away events don't reach the game (see eventFilter).
std::atomic<bool> handover{false};
/// The "going to the background" events kept from the game whose "back to the foreground" must be kept too.
std::atomic<int> droppedBackgrounds{0};
/// The window was resized while the pane handed over (the game is told once back on the pane, see backOnPane).
std::atomic<bool> droppedResize{false};

/**
 * Keeps the panel's going-away events from the game while VR takes over: the game would pause
 * itself (SDL_APP_WILLENTERBACKGROUND: the music paused and the sound locked; a minimised window:
 * the game slowed to a crawl) though it plays on in the headset. Each "background" kept away has
 * its "foreground" kept away too, so the game's pause and resume stay paired whenever they come.
 * A resize of the hidden panel waits for the pane too (the game would remake its renderer while
 * SDL has no surface to draw on). (It runs on whichever thread sends the event: Android's UI
 * thread, for these.)
 * @param event The event about to be queued.
 * @return 0 to drop it, 1 to queue it.
 */
int SDLCALL eventFilter(void *, SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_APP_WILLENTERBACKGROUND:
		if (handover)
		{
			++droppedBackgrounds;
			return 0;
		}
		return 1;
	case SDL_APP_WILLENTERFOREGROUND:
	{
		int dropped = droppedBackgrounds;
		while (dropped > 0)
		{
			if (droppedBackgrounds.compare_exchange_weak(dropped, dropped - 1))
			{
				return 0;
			}
		}
		return 1;
	}
	case SDL_APP_DIDENTERBACKGROUND:
	case SDL_APP_DIDENTERFOREGROUND:
		return handover ? 0 : 1;
	case SDL_WINDOWEVENT:
		if (handover && (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST || event->window.event == SDL_WINDOWEVENT_MINIMIZED
			|| event->window.event == SDL_WINDOWEVENT_HIDDEN))
		{
			return 0;
		}
		if (handover && (event->window.event == SDL_WINDOWEVENT_RESIZED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
		{
			droppedResize = true;
			return 0;
		}
		return 1;
	default:
		return 1;
	}
}

/// A line to the game's log and the system's (tag OpenXcom3D), starting "VR ".
void logBoth(const std::string &text)
{
	Log(LOG_INFO) << "OpenXcom3D: VR " << text;
	__android_log_print(ANDROID_LOG_INFO, "OpenXcom3D", "VR %s", text.c_str());
}

/// A line of the input log (the controllers' first uses: tag OpenXcom3DInput), to the system's log and the game's.
void logInput(const std::string &text)
{
	__android_log_print(ANDROID_LOG_INFO, "OpenXcom3DInput", "%s", text.c_str());
	Log(LOG_INFO) << "OpenXcom3DInput: " << text;
}

/// A line another thread handed to the game's log (see Xr::logToGame), and when (ms since the game began).
struct QueuedLogLine
{
	std::string text;
	Uint32 at;
};
/// The lines waiting for the game's thread (at most MaxQueuedLogLines: past that the oldest go).
std::mutex queuedLogMutex;
std::deque<QueuedLogLine> queuedLogLines;
const size_t MaxQueuedLogLines = 500;
/// A line written this long after it was handed over (the game's thread was held up: the panel paused) says when it
/// came (ms).
const Uint32 LateLogLineMs = 1000;

/// Writes the lines other threads handed over to the game's log (on the game's thread, every frame).
void writeQueuedLog()
{
	std::deque<QueuedLogLine> lines;
	{
		std::lock_guard<std::mutex> lock(queuedLogMutex);
		lines.swap(queuedLogLines);
	}
	const Uint32 now = SDL_GetTicks();
	for (const QueuedLogLine &line : lines)
	{
		if (now - line.at >= LateLogLineMs)
		{
			Log(LOG_INFO) << line.text << " (" << (now - line.at) << " ms ago)";
		}
		else
		{
			Log(LOG_INFO) << line.text;
		}
	}
}

/**
 * Calls a method, without arguments, of the game's panel activity (org.openxcom.extended.OpenXcom,
 * SDL's activity), from the game's thread.
 * @param method The method's name.
 * @return True when it was called.
 */
bool callPanelActivity(const char *method)
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject activity = env ? (jobject)SDL_AndroidGetActivity() : nullptr;
	if (!env || !activity)
	{
		logBoth(std::string("no panel activity for ") + method);
		return false;
	}
	jclass type = env->GetObjectClass(activity);
	jmethodID id = env->GetMethodID(type, method, "()V");
	bool called = false;
	if (id)
	{
		env->CallVoidMethod(activity, id);
		called = true;
	}
	if (env->ExceptionCheck())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		called = false;
	}
	env->DeleteLocalRef(type);
	env->DeleteLocalRef(activity);
	if (!called)
	{
		logBoth(std::string("the panel activity's ") + method + " failed");
	}
	return called;
}

/**
 * Calls a static method, without arguments, of the immersive activity
 * (org.openxcom.extended.VrActivity), from the game's thread.
 * @param method The method's name.
 * @return True when it was called.
 */
bool callVrActivity(const char *method)
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (!env)
	{
		return false;
	}
	jclass type = env->FindClass("org/openxcom/extended/VrActivity");
	bool called = false;
	if (type)
	{
		jmethodID id = env->GetStaticMethodID(type, method, "()V");
		if (id)
		{
			env->CallStaticVoidMethod(type, id);
			called = true;
		}
	}
	if (env->ExceptionCheck())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		called = false;
	}
	if (type)
	{
		env->DeleteLocalRef(type);
	}
	if (!called)
	{
		logBoth(std::string("the VR activity's ") + method + " failed");
	}
	return called;
}

/**
 * Is the game's panel back for SDL: its activity resumed for SDL (SDL's nativeResume sent) with its
 * surface ready? Asked of SDL's Java side (SDLActivity.isNativeResumed), from the game's thread.
 * @return True if so.
 */
bool panelShowing()
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (!env)
	{
		return false;
	}
	jclass type = env->FindClass("org/libsdl/app/SDLActivity");
	bool showing = false;
	if (type)
	{
		jmethodID id = env->GetStaticMethodID(type, "isNativeResumed", "()Z");
		if (id)
		{
			showing = env->CallStaticBooleanMethod(type, id) == JNI_TRUE;
		}
		env->DeleteLocalRef(type);
	}
	if (env->ExceptionCheck())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		showing = false;
	}
	return showing;
}

/**
 * An 8-bit picture through its palette into a layer's picture (RGBA, the bottom row first, as
 * OpenGL's textures have it).
 * @param picture The picture.
 * @param transparentZero True for the HUD: palette index 0 is see-through.
 * @param out The layer's picture.
 * @return False for an empty picture.
 */
bool convertIndexed(const VrPicture &picture, bool transparentZero, Xr::LayerPicture &out)
{
	if (!picture.pixels || !picture.palette || picture.width <= 0 || picture.height <= 0)
	{
		return false;
	}
	Uint32 colours[256];
	for (int i = 0; i < 256; ++i)
	{
		const SDL_Color &colour = picture.palette[i];
		colours[i] = (Uint32)colour.r | ((Uint32)colour.g << 8) | ((Uint32)colour.b << 16) | 0xFF000000u;
	}
	if (transparentZero)
	{
		colours[0] = 0;
	}
	out.width = picture.width;
	out.height = picture.height;
	out.pixels.resize((size_t)picture.width * picture.height);
	for (int y = 0; y < picture.height; ++y)
	{
		const Uint8 *from = picture.pixels + (size_t)y * picture.pitch;
		Uint32 *to = out.pixels.data() + (size_t)(picture.height - 1 - y) * picture.width;
		for (int x = 0; x < picture.width; ++x)
		{
			to[x] = colours[from[x]];
		}
	}
	return true;
}

/**
 * An eye's depth (the renderer's: how far ahead along the eye's view axis, in voxels; the sky
 * infinite) into the depth the headset takes (XR_KHR_composition_layer_depth): a standard
 * perspective projection's, 0 at DepthNearMetres, 1 at DepthFarMetres and beyond; the bottom row
 * first, as the pictures.
 * @param picture The eye's picture, with its depth.
 * @param out The depth handed over (emptied when the picture has none).
 */
void convertDepth(const VrPicture &picture, std::vector<float> &out)
{
	if (!picture.depth || picture.depthToMetres <= 0.0f || picture.width <= 0 || picture.height <= 0)
	{
		out.clear();
		return;
	}
	out.resize((size_t)picture.width * picture.height);
	for (int y = 0; y < picture.height; ++y)
	{
		const float *from = picture.depth + (size_t)y * picture.depthPitch;
		float *to = out.data() + (size_t)(picture.height - 1 - y) * picture.width;
		for (int x = 0; x < picture.width; ++x)
		{
			to[x] = vrProjectionDepth(from[x] * picture.depthToMetres);
		}
	}
}

/**
 * The game's whole screen (32-bit) into the monitor's picture: RGBA, the bottom row first, each
 * game pixel two by two (so the headset's smoothing keeps the pixels' edges sharp), or one by one
 * when twice would be over MonitorMaxSize (a big base resolution; swapchains have a limit).
 * @param screen The screen's surface.
 * @param out The monitor's picture.
 * @return False for a screen that isn't 32-bit.
 */
bool convertScreen(SDL_Surface *screen, Xr::LayerPicture &out)
{
	const SDL_PixelFormat *format = screen->format;
	if (format->BytesPerPixel != 4 || screen->w <= 0 || screen->h <= 0)
	{
		return false;
	}
	if (screen->w * 2 > MonitorMaxSize || screen->h * 2 > MonitorMaxSize)
	{
		// One game pixel to one picture pixel.
		out.width = screen->w;
		out.height = screen->h;
		out.pixels.resize((size_t)screen->w * screen->h);
		for (int y = 0; y < screen->h; ++y)
		{
			const Uint32 *from = (const Uint32 *)((const Uint8 *)screen->pixels + (size_t)y * screen->pitch);
			Uint32 *to = out.pixels.data() + (size_t)(screen->h - 1 - y) * screen->w;
			for (int x = 0; x < screen->w; ++x)
			{
				const Uint32 pixel = from[x];
				to[x] = ((pixel & format->Rmask) >> format->Rshift) | (((pixel & format->Gmask) >> format->Gshift) << 8)
					| (((pixel & format->Bmask) >> format->Bshift) << 16) | 0xFF000000u;
			}
		}
		return true;
	}
	const int width = screen->w * 2, height = screen->h * 2;
	out.width = width;
	out.height = height;
	out.pixels.resize((size_t)width * height);
	for (int y = 0; y < screen->h; ++y)
	{
		const Uint32 *from = (const Uint32 *)((const Uint8 *)screen->pixels + (size_t)y * screen->pitch);
		Uint32 *to = out.pixels.data() + (size_t)(height - 2 - 2 * y) * width;
		for (int x = 0; x < screen->w; ++x)
		{
			const Uint32 pixel = from[x];
			const Uint32 rgba = ((pixel & format->Rmask) >> format->Rshift) | (((pixel & format->Gmask) >> format->Gshift) << 8)
				| (((pixel & format->Bmask) >> format->Bshift) << 16) | 0xFF000000u;
			to[2 * x] = to[2 * x + 1] = rgba;
			to[width + 2 * x] = to[width + 2 * x + 1] = rgba;
		}
	}
	return true;
}

/**
 * Draws the free cursor onto the HUD's picture (the game draws its own cursor onto its screen, not
 * onto that picture): a yellow arrow with a black edge, its tip at the mouse's place.
 * @param hud The HUD's picture (RGBA, the bottom row first; the screen's size).
 * @param tipX The tip's distance from the left edge, in the picture's pixels.
 * @param tipY From the top.
 */
void drawHudCursor(Xr::LayerPicture &hud, int tipX, int tipY)
{
	const Uint32 fill = 0xFF00FFFFu, edge = 0xFF000000u; // (R, G, B, A in memory: yellow, black)
	const int height = 14;
	for (int row = 0; row < height; ++row)
	{
		const int width = 1 + row * 2 / 3;
		for (int column = 0; column <= width; ++column)
		{
			const int x = tipX + column, y = tipY + row;
			if (x < 0 || y < 0 || x >= hud.width || y >= hud.height)
			{
				continue;
			}
			const bool border = column == 0 || column == width || row == height - 1;
			hud.pixels[(size_t)(hud.height - 1 - y) * hud.width + x] = border ? edge : fill;
		}
	}
}

/// The game's window (SDL's, the panel's), where the controllers' events go.
SDL_Window *gameWindow()
{
	Game *game = State::getGame();
	return game && game->getScreen() ? game->getScreen()->getWindow() : nullptr;
}

/**
 * The battle's screen, or null (no battle on).
 */
BattlescapeState *battleState()
{
	Game *game = State::getGame();
	if (!game || !game->getSavedGame() || !game->getSavedGame()->getSavedBattle())
	{
		return nullptr;
	}
	return game->getSavedGame()->getSavedBattle()->getBattleState();
}

/**
 * Is the 3D view's cursor free (mouse look off: the add-on's key, C on the PC), for the top bar's
 * buttons? (Mouse look on, the game captures the mouse; without the 3D view, nothing is free.)
 */
bool cursorFree()
{
	const BattlescapeState *battle = battleState();
	return battle && vrStereoReady() && !battle->wantsMouseCapture();
}

/**
 * Where a point of the game's screen is in the window's pixels (as the game maps a mouse: its scale
 * and black bands).
 * @param x The point's distance from the screen's left edge, in game pixels; set to the window's.
 * @param y From the top, the same way.
 */
void screenToWindow(double &x, double &y)
{
	const Screen *gameScreen = State::getGame()->getScreen();
	x = x * gameScreen->getXScale() + gameScreen->getCursorLeftBlackBand();
	y = y * gameScreen->getYScale() + gameScreen->getCursorTopBlackBand();
}

/**
 * Is the battle the screen on top now (no inventory, message, menu or other screen over it)?
 */
bool battleOnTop()
{
	Game *game = State::getGame();
	if (!game || !game->getSavedGame() || !game->getSavedGame()->getSavedBattle())
	{
		return false;
	}
	BattlescapeState *battle = game->getSavedGame()->getSavedBattle()->getBattleState();
	return battle && game->isState(battle);
}

/// Holds a key down for the controllers (or lets it go), as a keyboard would.
void holdKey(SDL_Keycode key, bool down)
{
	const SDL_Scancode code = key == SDLK_UNKNOWN ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromKey(key);
	if (code == SDL_SCANCODE_UNKNOWN || shell.input.keyHeld[code] == down)
	{
		return;
	}
	shell.input.keyHeld[code] = down;
	SDL_SendKeyboardKey(down ? SDL_PRESSED : SDL_RELEASED, code);
}

/// Presses a key once (down and up), as a keyboard would.
void pressKey(SDL_Keycode key)
{
	holdKey(key, true);
	holdKey(key, false);
}

/// Holds a mouse button down for the controllers (or lets it go), as a mouse would.
void holdButton(Uint8 button, bool down)
{
	SDL_Window *window = gameWindow();
	if (!window || button >= 8 || shell.input.buttonHeld[button] == down)
	{
		return;
	}
	shell.input.buttonHeld[button] = down;
	SDL_SendMouseButton(window, 0, down ? SDL_PRESSED : SDL_RELEASED, button);
}

/// Lets go of every key and button the controllers hold (leaving VR, the system's menu, a change of screen).
void releaseAll()
{
	for (int code = 0; code < SDL_NUM_SCANCODES; ++code)
	{
		if (shell.input.keyHeld[code])
		{
			shell.input.keyHeld[code] = false;
			SDL_SendKeyboardKey(SDL_RELEASED, (SDL_Scancode)code);
		}
	}
	for (Uint8 button = 0; button < 8; ++button)
	{
		holdButton(button, false);
	}
	shell.input.yHeldSince = 0;
}

/// Notes a button's first use in the input log (tag OpenXcom3DInput), with what it does there.
void logButton(int button, const char *what)
{
	if (!shell.input.logged[button])
	{
		shell.input.logged[button] = true;
		static const char *const names[Xr::ButtonCount] = { "left trigger", "right trigger", "left grip", "right grip", "A", "B", "X", "Y", "menu", "left stick click", "right stick click" };
		logInput(std::string("VR controller: ") + names[button] + " (" + what + ")");
	}
}

/// Was the button pressed (since the last frame, or still held as it came down)? Logs its first use.
bool pressed(const Xr::ControllerState &controllers, int button, const char *what)
{
	if (controllers.presses[button] > 0)
	{
		logButton(button, what);
		return true;
	}
	return false;
}

/**
 * A mouse button from a controller button: down while it's held, and a click for a press too
 * short to be seen held between two of the game's frames.
 */
void buttonFrom(const Xr::ControllerState &controllers, int button, Uint8 mouseButton, const char *what)
{
	const bool wasPressed = pressed(controllers, button, what);
	if (wasPressed && !controllers.held[button] && !shell.input.buttonHeld[mouseButton])
	{
		holdButton(mouseButton, true);
		holdButton(mouseButton, false);
		return;
	}
	holdButton(mouseButton, controllers.held[button]);
}

/**
 * The controllers in the 3D view: the left stick walks and strafes (the movement keys held), the
 * right stick goes up or down (their keys; its push sideways turns: see feedImmersive), the right trigger fires
 * (the left mouse button; with the cursor free, it clicks), the left trigger frees the cursor or
 * gives mouse look back (the add-on's key, C on the PC), and with the cursor free the right
 * controller's ray moves it over the HUD's panel (the top bar's buttons); the right grip is the
 * right mouse button, A the inventory, B back to the pane (F8), X kneels, Y held a second ends
 * the turn, the left stick's click sprints, the right stick's click recentres, the menu button is
 * Esc.
 * @param controllers The controllers.
 * @param screen The game's screen (its size: the HUD's picture is the screen's).
 */
void controlsIn3D(const Xr::ControllerState &controllers, SDL_Surface *screen)
{
	ControllerInput &input = shell.input;
	const Uint32 now = SDL_GetTicks();

	// Walking: the movement keys held while the stick is pushed half-way (the 3D view's own walking rules apply).
	holdKey(Options::keyFpsForward, controllers.move[1] > StickWalk);
	holdKey(Options::keyFpsBack, controllers.move[1] < -StickWalk);
	holdKey(Options::keyFpsStrafeRight, controllers.move[0] > StickWalk);
	holdKey(Options::keyFpsStrafeLeft, controllers.move[0] < -StickWalk);

	// The right stick: its push sideways turns the soldier (handed to the renderer every frame: see feedImmersive); a
	// flick up or down (more up than sideways) goes up or down, again once back near the middle.
	if (std::fabs(controllers.turn[1]) < StickRearm)
	{
		input.upDownReady = true;
	}
	if (input.upDownReady && std::fabs(controllers.turn[1]) > StickFlick && std::fabs(controllers.turn[1]) > std::fabs(controllers.turn[0]))
	{
		input.upDownReady = false;
		pressKey(controllers.turn[1] > 0.0f ? Options::keyFpsUp : Options::keyFpsDown);
	}

	// The left trigger: mouse look on or off (the add-on's key, C on the PC), once a pull. With the cursor free, the
	// right controller's ray moves it over the HUD's panel (the HUD's picture is the screen's: its top bar's buttons
	// are where the game has them), in the window's pixels as the game maps a mouse.
	if (pressed(controllers, Xr::ButtonTriggerLeft, "mouse look / free cursor (the C key)"))
	{
		pressKey(Options::keyFpsMouseMode);
	}
	SDL_Window *window = gameWindow();
	if (window && cursorFree() && controllers.rayOnHud)
	{
		double x = controllers.hudX * screen->w, y = controllers.hudY * screen->h;
		screenToWindow(x, y);
		if ((int)x != input.mouseX || (int)y != input.mouseY)
		{
			input.mouseX = (int)x;
			input.mouseY = (int)y;
			SDL_SendMouseMotion(window, 0, 0, input.mouseX, input.mouseY);
			if (!input.hudRayLogged)
			{
				input.hudRayLogged = true;
				logInput("VR controller: the right ray on the HUD (the free cursor)");
			}
		}
	}

	// The right trigger fires (the left mouse button; with the cursor free, a click where it is), the right grip is the
	// right button.
	buttonFrom(controllers, Xr::ButtonTriggerRight, SDL_BUTTON_LEFT, "the left mouse button: fire, or click with the cursor free");
	buttonFrom(controllers, Xr::ButtonGripRight, SDL_BUTTON_RIGHT, "the right mouse button");

	// The buttons: the game's own keys.
	if (pressed(controllers, Xr::ButtonA, "the inventory")) pressKey(Options::keyBattleInventory);
	if (pressed(controllers, Xr::ButtonB, "back to the pane: the 3D view's key")) pressKey(Options::keyBattleFirstPerson);
	if (pressed(controllers, Xr::ButtonX, "kneel")) pressKey(Options::keyBattleKneel);
	if (pressed(controllers, Xr::ButtonLeftStick, "sprint")) pressKey(Options::keyFpsSprint);
	if (pressed(controllers, Xr::ButtonMenu, "Esc: the game's menu")) pressKey(SDLK_ESCAPE);
	if (pressed(controllers, Xr::ButtonRightStick, "recentre"))
	{
		vrRecentre();
		Xr::shared().placeQuads = true;
		logBoth("recentred (the right stick's click)");
	}

	// Y held a second ends the turn (once a hold).
	if (controllers.held[Xr::ButtonY])
	{
		if (!input.yHeldSince)
		{
			input.yHeldSince = now;
			input.endTurnSent = false;
		}
		if (!input.endTurnSent && now - input.yHeldSince >= EndTurnHoldMs)
		{
			input.endTurnSent = true;
			logButton(Xr::ButtonY, "held: end the turn");
			pressKey(Options::keyBattleEndTurn);
		}
	}
	else
	{
		input.yHeldSince = 0;
	}
}

/**
 * The controllers on the monitor (another screen over the battle): the right controller's ray
 * moves the mouse, the right trigger is the left button, the right grip the right button, the
 * right stick up and down the wheel, A the inventory key (it closes the inventory), B and the
 * menu button Esc; the left trigger nothing (the cursor is free there anyway, and C would be the
 * inventory's template key).
 * @param controllers The controllers.
 * @param screen The game's screen (its size, for the mouse's place).
 */
void controlsOnMonitor(const Xr::ControllerState &controllers, SDL_Surface *screen)
{
	ControllerInput &input = shell.input;
	SDL_Window *window = gameWindow();
	Game *game = State::getGame();
	if (!window || !game)
	{
		return;
	}
	// Walking stops (a stick held as the screen came up).
	holdKey(Options::keyFpsForward, false);
	holdKey(Options::keyFpsBack, false);
	holdKey(Options::keyFpsStrafeRight, false);
	holdKey(Options::keyFpsStrafeLeft, false);

	// The ray: the mouse to where it meets the monitor, in the window's pixels (as the game maps a mouse).
	if (controllers.rayOnMonitor)
	{
		const Screen *gameScreen = game->getScreen();
		const int x = (int)(controllers.monitorX * screen->w * gameScreen->getXScale()) + gameScreen->getCursorLeftBlackBand();
		const int y = (int)(controllers.monitorY * screen->h * gameScreen->getYScale()) + gameScreen->getCursorTopBlackBand();
		if (x != input.mouseX || y != input.mouseY)
		{
			input.mouseX = x;
			input.mouseY = y;
			SDL_SendMouseMotion(window, 0, 0, x, y);
			if (!input.monitorLogged)
			{
				input.monitorLogged = true;
				logInput("VR controller: the right ray on the monitor (the mouse)");
			}
		}
	}

	// The buttons.
	buttonFrom(controllers, Xr::ButtonTriggerRight, SDL_BUTTON_LEFT, "the monitor's left button");
	buttonFrom(controllers, Xr::ButtonGripRight, SDL_BUTTON_RIGHT, "the monitor's right button");
	if (pressed(controllers, Xr::ButtonA, "the inventory key")) pressKey(Options::keyBattleInventory);
	if (pressed(controllers, Xr::ButtonB, "Esc on the monitor")) pressKey(SDLK_ESCAPE);
	if (pressed(controllers, Xr::ButtonMenu, "Esc on the monitor")) pressKey(SDLK_ESCAPE);

	// The wheel: a notch as the stick is pushed, then every WheelRepeatMs while it's held.
	const Uint32 now = SDL_GetTicks();
	if (std::fabs(controllers.turn[1]) > StickFlick)
	{
		if (input.upDownReady || now >= input.wheelNextAt)
		{
			input.upDownReady = false;
			input.wheelNextAt = now + WheelRepeatMs;
			SDL_SendMouseWheel(window, 0, 0.0f, controllers.turn[1] > 0.0f ? 1.0f : -1.0f, SDL_MOUSEWHEEL_NORMAL);
		}
	}
	else if (std::fabs(controllers.turn[1]) < StickRearm)
	{
		input.upDownReady = true;
	}
}

/**
 * Every ProbeSeconds, the game thread's side of VR to both logs: the eye pairs drawn and how long
 * a pair took (the 3D view's cost in VR, the headset test's main number), the monitor's and the
 * HUD's pictures, and the game's frames.
 */
void reportProbe()
{
	const Uint32 now = SDL_GetTicks();
	if (!shell.probeSince)
	{
		shell.probeSince = now;
		return;
	}
	if (now - shell.probeSince < ProbeSeconds * 1000u)
	{
		return;
	}
	const double seconds = (now - shell.probeSince) / 1000.0;
	std::ostringstream report;
	report.setf(std::ios::fixed);
	report.precision(1);
	report << "eyes: " << shell.probePairs << " pairs in " << seconds << " s (" << shell.probePairs / seconds << " a second)";
	if (shell.probePairs)
	{
		const double average = shell.probeTotalMs / shell.probePairs;
		report << ", drawing a pair " << average << " ms on average (" << average / 2.0 << " an eye), the slowest " << shell.probeWorstMs << " ms"
			<< ", handing it over " << shell.probeHandOverMs / shell.probePairs << " ms";
	}
	report << "; the monitor " << shell.probeMonitors << " times, the HUD " << shell.probeHuds << " times; game frames " << shell.probeFrames;
	logBoth(report.str());
	shell.probeSince = now;
	shell.probePairs = shell.probeMonitors = shell.probeHuds = shell.probeFrames = 0;
	shell.probeTotalMs = shell.probeWorstMs = shell.probeHandOverMs = 0.0;
}

/**
 * A frame in VR: the headset's recentring followed, the newest head pose to the 3D view, both
 * eyes drawn for the newest views and handed over (only with the battle on top), the HUD, the
 * game's whole screen for the monitor when another screen is over the battle, and the
 * controllers turned into the game's keys and mouse.
 * @param screen The game's screen, as finished this frame.
 */
void feedImmersive(SDL_Surface *screen)
{
	Xr::Shared &box = Xr::shared();
	++shell.probeFrames;

	// The headset recentred: so does the 3D view.
	const int recentres = box.recentres;
	if (recentres != shell.recentresSeen)
	{
		shell.recentresSeen = recentres;
		vrRecentre();
	}

	// The newest head and eyes, and the controllers (their presses taken: they act below, after the eyes).
	Xr::HeadSample sample;
	{
		std::lock_guard<std::mutex> lock(box.sampleMutex);
		sample = box.sample;
	}
	Xr::ControllerState controllers;
	{
		std::lock_guard<std::mutex> lock(box.controllerMutex);
		controllers = box.controllers;
		std::fill(std::begin(box.controllers.presses), std::end(box.controllers.presses), 0);
	}
	const bool battle = battleOnTop();
	if (sample.valid)
	{
		vrSetHeadPose(sample.head);
		// The weapon in the right hand (its aim and grip, the left hand's aim, the left grip for two hands), and the right
		// stick's push sideways: the only thing that turns the soldier (the renderer's VR turning, smooth or snap; the
		// turn keys no longer turn). Nothing turns while another screen is up or the system's menu has the input.
		const auto handOf = [&sample](int hand) { return sample.handTracked[hand] ? &sample.hands[hand] : nullptr; };
		vrSetHands(handOf(Xr::HandRightAim), handOf(Xr::HandRightGrip), handOf(Xr::HandLeftAim), controllers.focused && controllers.held[Xr::ButtonGripLeft]);
		vrSetTurnAxis(battle && controllers.focused ? controllers.turn[0] : 0.0);
	}

	// The eyes and the HUD: drawn with the battle on top; under another screen the last ones stay (and none is drawn
	// while the session doesn't run: the headset taken off).
	bool hudShown = false;
	if (battle && sample.valid && box.running && vrStereoReady())
	{
		int width = 0, height = 0;
		vrEyeSize(width, height);
		VrPicture pictures[2];
		// Kept on the graphics card (setting "VR eyes kept on the card", with "Renderer": GPU), where the OpenXR thread's
		// context shares the renderer's textures: in the slot the pair goes over in. Else they come back as pictures.
		FirstPersonGpu *const gpu = Options::fpsRenderer == 1 ? readyFirstPersonGpu() : nullptr;
		const bool onCard = Options::fpsVrCardEyes && box.cardEyes && gpu && gpu->sharesContext();
#ifdef OPENXCOM3D_XR_FAKE_SESSION
		// (The emulator test build's check, asked for by a file: both ways, for the OpenXR thread to compare.)
		static const bool cardCheck = access("/sdcard/openxcom3d/vrtest_card_check", F_OK) == 0;
#else
		const bool cardCheck = false;
#endif
		const Uint64 start = SDL_GetPerformanceCounter();
		const bool drawn = vrRenderEyes(sample.views, width, height, pictures, nullptr, onCard ? box.eyes.backIndex() : -1, onCard && cardCheck);
		const double drawMs = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
		if (drawn)
		{
			Xr::EyePictures &out = box.eyes.back();
			out.onCard = pictures[0].colorTexture && pictures[1].colorTexture && pictures[0].fence;
			out.cardFence = pictures[0].fence;
			bool ready = out.onCard;
			if (pictures[0].pixels && pictures[1].pixels)
			{
				ready = convertIndexed(pictures[0], false, out.eyes[0]) && convertIndexed(pictures[1], false, out.eyes[1]);
			}
			if (shell.eyesOnCard != (out.onCard ? 1 : 0))
			{
				shell.eyesOnCard = out.onCard ? 1 : 0;
				logBoth(out.onCard ? "eyes: kept on the graphics card, shown from its textures (nothing read back or uploaded)"
					: "eyes: handed over as pictures (read back from the graphics card, or the CPU's)");
			}
			if (ready)
			{
				for (int eye = 0; eye < 2; ++eye)
				{
					out.views[eye].pose = sample.views[eye].pose;
					out.views[eye].fov = pictures[eye].fov;
					out.cardColour[eye] = pictures[eye].colorTexture;
					out.cardDepth[eye] = pictures[eye].depthTexture;
					if (pictures[eye].pixels)
					{
						convertDepth(pictures[eye], out.depth[eye]);
					}
					else
					{
						// (Only its size: the picture is the card's.)
						out.eyes[eye].width = pictures[eye].width;
						out.eyes[eye].height = pictures[eye].height;
						out.eyes[eye].pixels.clear();
						out.depth[eye].clear();
					}
				}
				box.eyes.publish();
				++shell.probePairs;
				shell.probeHandOverMs += (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency() - drawMs;
				shell.probeTotalMs += drawMs;
				shell.probeWorstMs = std::max(shell.probeWorstMs, drawMs);
			}
		}
		VrPicture hud;
		if (vrHudPicture(hud) && convertIndexed(hud, true, box.hud.back()))
		{
			// The free cursor (mouse look off) drawn where the mouse is: the HUD's picture is the screen's.
			int windowX = 0, windowY = 0;
			Screen *gameScreen = State::getGame()->getScreen();
			if (cursorFree() && hud.width == screen->w && hud.height == screen->h && gameScreen->getXScale() > 0.0 && gameScreen->getYScale() > 0.0)
			{
				SDL_GetMouseState(&windowX, &windowY);
				drawHudCursor(box.hud.back(), (int)((windowX - gameScreen->getCursorLeftBlackBand()) / gameScreen->getXScale()),
					(int)((windowY - gameScreen->getCursorTopBlackBand()) / gameScreen->getYScale()));
			}
			box.hud.publish();
			hudShown = true;
			++shell.probeHuds;
		}
	}

	// The monitor: the game's whole screen, whenever another screen is over the battle.
	const bool monitorShown = !battle;
	if (monitorShown && convertScreen(screen, box.monitor.back()))
	{
		box.monitor.publish();
		++shell.probeMonitors;
	}
	box.showHud = hudShown;
	box.showMonitor = monitorShown;
	// The HUD holds still in the room while the cursor is free (the right ray clicks its buttons), and the eyes are
	// smoothed by the graphics chip as the setting says.
	box.hudHeld = battle && cursorFree();
	box.smoothing = Options::fpsVrSmoothing;
	box.cardEyesWanted = Options::fpsVrCardEyes && Options::fpsRenderer == 1;

	// The log: another screen coming over the battle and going, the cursor freed and mouse look back.
	ControllerInput &input = shell.input;
	if ((monitorShown ? 1 : 0) != input.monitorWas)
	{
		if (input.monitorWas != -1)
		{
			logBoth(monitorShown ? "another screen over the battle: on the monitor" : "the battle again");
		}
		input.monitorWas = monitorShown ? 1 : 0;
	}
	const int freeNow = cursorFree() ? 1 : 0;
	if (battle && freeNow != input.cursorFreeWas)
	{
		if (input.cursorFreeWas != -1)
		{
			logBoth(freeNow ? "the cursor free (mouse look off): the right ray moves it over the HUD" : "mouse look on");
		}
		input.cursorFreeWas = freeNow;
	}

	// The controllers: under the system's menu nothing is held; the monitor's controls while another screen is up.
	if (!controllers.focused)
	{
		releaseAll();
	}
	else if (battle)
	{
		controlsIn3D(controllers, screen);
	}
	else
	{
		controlsOnMonitor(controllers, screen);
	}
	reportProbe();
}

/**
 * Goes from the pane to VR: SDL keeps the game running and its sound playing while the panel is
 * in the background, the panel's going-away events are kept from the game, and the panel starts
 * the immersive activity (whose creation starts the OpenXR session).
 * @return False when the immersive activity couldn't be asked for (the pane stays).
 */
bool enterImmersive()
{
	if (Xr::sessionThreadAlive())
	{
		logBoth("the last VR session is still ending: the pane stays this time");
		return false;
	}
	logBoth("the 3D view in VR mode: starting the immersive activity");
	Xr::Shared &box = Xr::shared();
	box.gameThreadId = (int)gettid();
	box.cardEyesWanted = Options::fpsVrCardEyes && Options::fpsRenderer == 1; // (read when the session's context is made)
	box.finished = false;
	box.failed = false;
	box.running = false;
	box.stopRequested = false;
	SDL_SetHint(HintBlockOnPause, "0");
	SDL_SetHint(HintPauseAudio, "0");
	handover = true;
	if (!callPanelActivity("startVrActivity"))
	{
		handover = false;
		SDL_SetHint(HintBlockOnPause, "1");
		SDL_SetHint(HintPauseAudio, "1");
		return false;
	}
	return true;
}

/// Starts leaving VR: the session is asked to end; the panel is asked for in the Leaving state.
void leaveImmersive(const char *why)
{
	logBoth(std::string("leaving VR: ") + why);
	releaseAll();
	Xr::shared().stopRequested = true;
	Xr::shared().showHud = false;
	Xr::shared().showMonitor = false;
	shell.state = ShellState::Leaving;
	shell.stateSince = SDL_GetTicks();
	shell.panelAsked = false;
	shell.panelBackFrame = 0;
}

/// Back on the pane: SDL blocks the game in the background again, the panel's events reach the game again (and a
/// resize kept back meanwhile now reaches it, with the window's size now); the display's reset waits for the pane's
/// first frame.
void backOnPane()
{
	SDL_SetHint(HintBlockOnPause, "1");
	SDL_SetHint(HintPauseAudio, "1");
	handover = false;
	SDL_Window *window = gameWindow();
	if (droppedResize.exchange(false) && window)
	{
		SDL_Event resized;
		SDL_zero(resized);
		resized.type = SDL_WINDOWEVENT;
		resized.window.event = SDL_WINDOWEVENT_RESIZED;
		resized.window.windowID = SDL_GetWindowID(window);
		SDL_GetWindowSize(window, &resized.window.data1, &resized.window.data2);
		SDL_PushEvent(&resized);
	}
	// ...and the game's running state back to running, whatever the panel's comings and goings left it in (as SDL's own
	// "restored" does): the pane's frames are drawn by themselves from now on, no input needed.
	if (window)
	{
		SDL_Event restored;
		SDL_zero(restored);
		restored.type = SDL_WINDOWEVENT;
		restored.window.event = SDL_WINDOWEVENT_RESTORED;
		restored.window.windowID = SDL_GetWindowID(window);
		SDL_PushEvent(&restored);
	}
	callPanelActivity("vrEnded");
	std::ostringstream text;
	text << "back on the pane after " << SDL_GetTicks() - shell.stateSince << " ms";
	logBoth(text.str());
	shell.state = ShellState::Pane;
	shell.stateSince = SDL_GetTicks();
	shell.probeSince = 0;
	// What the game changed of its display meanwhile (another screen's own size, as the inventory's) reaches SDL's
	// renderer at the pane's first frame: the next, after SDL's event pump (which waits while the panel is away and
	// gives the game its drawing context back), not this one (the panel may be on its way still).
	shell.displayResetDue = true;
}

#ifdef OPENXCOM3D_XR_FAKE_SESSION
/**
 * THE EMULATOR TEST BUILD ONLY: does a file in the game's profile folder (/sdcard/openxcom3d) hold a word?
 * @param name The file's name.
 * @param word The word.
 */
bool testFileSays(const char *name, const char *word)
{
	const std::string path = std::string("/sdcard/openxcom3d/") + name;
	FILE *file = std::fopen(path.c_str(), "r");
	if (!file)
	{
		return false;
	}
	char line[64] = "";
	const bool says = std::fgets(line, sizeof(line), file) && std::strstr(line, word);
	std::fclose(file);
	return says;
}

/// THE EMULATOR TEST BUILD ONLY: a segfault on purpose, in a function of its own (the crash log's check: its frames
/// should name this function and its caller).
[[gnu::noinline]] void testCrashOnPurpose()
{
	volatile int *nowhere = nullptr;
	*nowhere = 115;
}

/**
 * THE EMULATOR TEST BUILD ONLY, every frame: the file /sdcard/openxcom3d/vrtest_vrmode_off (the emulator script's)
 * switches the setting VR mode off, once (the owner's variant: the pane's own 3D view after a round trip in VR); the
 * file vrtest_crash crashes the game (the crash log's check).
 */
void testBuildFrame()
{
	if (shell.frames % 30 == 0 && std::remove("/sdcard/openxcom3d/vrtest_vrmode_off") == 0)
	{
		Options::fpsVrMode = 0;
		logBoth("TEST BUILD: VR mode switched off");
	}
	if (shell.frames % 30 == 15 && std::remove("/sdcard/openxcom3d/vrtest_crash") == 0)
	{
		logBoth("TEST BUILD: a crash on purpose (the crash log's check)");
		testCrashOnPurpose();
	}
}
#endif

}

namespace Xr
{

/**
 * Hands a line to the game's log, from any thread (the OpenXR thread, Android's UI thread): the game's thread writes it
 * at its next frame (see writeQueuedLog).
 * @param line The line.
 */
void logToGame(const std::string &line)
{
	std::lock_guard<std::mutex> lock(queuedLogMutex);
	if (queuedLogLines.size() >= MaxQueuedLogLines)
	{
		queuedLogLines.pop_front();
	}
	queuedLogLines.push_back({ line, SDL_GetTicks() });
}

}

/**
 * Does VR mode hold the display now (from asking for the immersive activity until the pane is
 * back)?
 */
bool xrShellHoldsDisplay()
{
	return shell.state != ShellState::Pane;
}

/**
 * Hands the frame just finished to VR mode, and does VR's work for the frame (see the file's
 * comment for the states).
 * @param screen The game's whole screen, 32-bit.
 * @return True while the headset shows the game (the pane isn't drawn).
 */
bool xrShellFlip(SDL_Surface *screen)
{
	if (!shell.filterInstalled)
	{
		shell.filterInstalled = true;
		SDL_SetEventFilter(eventFilter, nullptr);
	}
	const Uint32 now = SDL_GetTicks();
	++shell.frames;
	// The lines other threads handed to the game's log.
	writeQueuedLog();
#ifdef OPENXCOM3D_XR_FAKE_SESSION
	testBuildFrame();
#endif

	// Is the player in the 3D view with VR mode on? The renderer's vrInThreeD: from F8 in to F8 out on his turns, through
	// 2D screens and untracked moments; off for the aliens' turn (it plays on the pane, on the map) and back by itself
	// with his next turn. (Gone only after LeaveGraceMs; once gone, VR may be tried again.)
	const bool threeD = vrInThreeD();
	if (threeD)
	{
		shell.threeDLostAt = 0;
	}
	else if (!shell.threeDLostAt)
	{
		shell.threeDLostAt = now ? now : 1;
	}
	const bool threeDGone = !threeD && now - shell.threeDLostAt > LeaveGraceMs;
	if (threeDGone)
	{
		shell.declined = false;
	}

	Xr::Shared &box = Xr::shared();
	switch (shell.state)
	{
	case ShellState::Pane:
		// The pane's first frame after VR: the display's reset (see backOnPane), before anything is drawn on it.
		if (shell.displayResetDue)
		{
			shell.displayResetDue = false;
			Game *game = State::getGame();
			if (game && game->getScreen())
			{
				game->getScreen()->resetDisplay(false);
			}
		}
		if (threeD && !shell.declined)
		{
			if (enterImmersive())
			{
				shell.state = ShellState::Entering;
				shell.stateSince = now;
				shell.recentresSeen = box.recentres;
				return true;
			}
			shell.declined = true;
		}
		return false;

	case ShellState::Entering:
		feedImmersive(screen);
		if (box.running)
		{
			std::ostringstream text;
			text << "in the headset after " << now - shell.stateSince << " ms";
			logBoth(text.str());
			shell.state = ShellState::Immersive;
			shell.stateSince = now;
		}
		else if (box.failed || box.finished || now - shell.stateSince > EnterTimeoutMs)
		{
			shell.declined = true;
			leaveImmersive(box.failed ? "OpenXR didn't start" : "the immersive activity didn't come");
		}
		return true;

	case ShellState::Immersive:
		// (The headset taken off only stops the session for a while; the thread ending is the end.)
		if (box.finished)
		{
			shell.declined = true;
			leaveImmersive("the headset's session ended");
			return true;
		}
		if (threeDGone)
		{
			leaveImmersive("the 3D view was left");
			return true;
		}
		feedImmersive(screen);
		return true;

	case ShellState::Leaving:
		releaseAll();
		if (!shell.panelAsked && (box.finished || now - shell.stateSince > PanelAskMs))
		{
			shell.panelAsked = true;
			callVrActivity("returnToPanel");
		}
		// The pane once the panel is back: SDL has it again (its activity resumed for SDL, with its surface: see
		// panelShowing), its "back to the foreground" has come for each "to the background" (none came if it never
		// went), and SDL's event pump has run once since (it restores SDL's surface and context at the start of a
		// frame). PanelWaitMs at most: then the pane anyway (if the panel is still away, SDL blocks the game until it
		// comes back, as for any app in the background).
		if (shell.panelAsked && !shell.panelBackFrame && droppedBackgrounds == 0 && panelShowing())
		{
			shell.panelBackFrame = shell.frames;
		}
#ifdef OPENXCOM3D_XR_FAKE_SESSION
		// The emulator test build: vrtest_panel.txt saying "early" brings the pane back as soon as the panel was asked
		// for, as the headset did (build 114's report: 99 ms, the panel coming 3.5 s later), the panel's own events
		// reaching the game from then on.
		if (shell.panelAsked && !shell.panelBackFrame && testFileSays("vrtest_panel.txt", "early"))
		{
			logBoth("TEST BUILD: the pane at once, as on the headset");
			droppedBackgrounds = 0;
			shell.panelBackFrame = shell.frames;
		}
#endif
		if ((shell.panelBackFrame && shell.frames > shell.panelBackFrame) || (shell.panelAsked && now - shell.stateSince > PanelWaitMs))
		{
			backOnPane();
			return true; // (the pane's first frame is the next: see backOnPane)
		}
		return true;
	}
	return false;
}

namespace Xr
{

void *rendererShareContext()
{
	return FirstPersonGpu::shareContext();
}

}

}

}

/// The panel's input kinds (org.openxcom.extended.OpenXcom.logInputKind, on Android's UI thread) to the game's log too.
extern "C" JNIEXPORT void JNICALL Java_org_openxcom_extended_OpenXcom_nativeLogInput(JNIEnv *env, jclass, jstring kind)
{
	const char *text = env->GetStringUTFChars(kind, nullptr);
	if (text)
	{
		OpenXcom::FirstPerson::Xr::logToGame(std::string("OpenXcom3DInput: ") + text);
		env->ReleaseStringUTFChars(kind, text);
	}
}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
