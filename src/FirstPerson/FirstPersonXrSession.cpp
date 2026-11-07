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
 * FirstPersonXrSession.cpp: VR mode's OpenXR thread on the Meta Quest (Android only; see
 * FirstPersonXr.h for how it and the game's thread work together).
 *
 * The immersive activity (org.openxcom.extended.VrActivity, in the Android wrapper) calls
 * startSession when it is created and stopSession when it goes. In between, this thread:
 *
 *   1. starts OpenXR through Khronos's loader (XR_KHR_android_create_instance, with the activity),
 *      makes an OpenGL ES 3 context of its own (an EGL context with a tiny off-screen surface:
 *      SDL's context stays the game's) and a session on it, the LOCAL space (its origin where the
 *      head was at the last recentring: the soldier's eye, seated or standing) and the VIEW space
 *      (the head);
 *   2. sets up the controllers as OpenXR actions (the Touch controllers' buttons, sticks and the
 *      right one's aim), which the game's thread turns into the game's keys and mouse;
 *   3. runs the frames at the headset's rate: xrWaitFrame, xrBeginFrame, the controllers and the
 *      head and eyes read for that frame's display time and handed over, new pictures uploaded
 *      (glTexSubImage2D into the layers' swapchains), and xrEndFrame with the layers, back to
 *      front: the eyes (a projection layer, each eye's picture with the pose it was drawn for),
 *      the monitor (a quad with the game's whole screen, when another screen is over the
 *      battle) and the HUD (a quad, transparent where empty, fixed where the head looks);
 *   4. ends the session when asked (the game left the 3D view) or when the system ends it.
 *
 * Passthrough is never used: the 3D view is fully immersive, and with passthrough on the headset
 * would hold its CPU at a lower level (the study, section 3).
 *
 * Everything goes to the log (tag OpenXcom3D, lines starting "VR"; the game's log too): the runtime
 * and the headset, every OpenXR call that fails, the session's states, and every few seconds the
 * frames, how long each waited, and how many new eye pictures came.
 */
#ifdef __ANDROID__
#include "FirstPersonXr.h"
#include "FirstPersonXrSmoothing.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <android/log.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace OpenXcom
{

namespace FirstPerson
{

namespace Xr
{

namespace
{

/// The monitor (the game's whole screen, shown while another screen is over the battle): its width, how far in front
/// of the head it stands and how far below the eyes its middle is, in metres (2 m wide at 2 m: 53 degrees, a game
/// pixel about two of the headset's).
const float MonitorWidth = 2.0f, MonitorDistance = 2.0f, MonitorDrop = 0.1f;
/// The HUD's panel: this wide, in degrees (the owner, after 116's 60: "juuust a bit smaller"), at VrHudDistance.
const float HudWidthDegrees = 54.0f;
/// The filter that smooths the eyes (setting "VR smoothing"; FirstPersonXrSmoothing.h): SABR, the pixel art scaler,
/// chosen over FSR 1 on the game's pictures (quest3/shots/smoothing: its edges come out as clean lines, FSR's soft steps).
const SmoothingFilter EyeSmoothingFilter = SmoothingFilter::Sabr;
/// GL_EXT_disjoint_timer_query's names (the smoothing's GPU time; not in GLES3's header).
const GLenum TimeElapsedQuery = 0x88BF /* GL_TIME_ELAPSED_EXT */, GpuDisjoint = 0x8FBB /* GL_GPU_DISJOINT_EXT */;
/// The frame statistics go to the log this often (seconds), as the game's frame probe does.
const int StatsSeconds = 5;
/// Asked to end, the session gets this long to end by itself before the thread leaves anyway (ms).
const int ExitTimeoutMs = 3000;
/// OpenGL ES's sRGB format (what the compositor should get our colours as) and the plain one (the fallback).
const int64_t FormatSrgb = 0x8C43 /* GL_SRGB8_ALPHA8 */, FormatRgba = 0x8058 /* GL_RGBA8 */;
/// The depth formats the eyes' depth may go in, the most precise first.
const int64_t FormatDepth32F = 0x8CAC /* GL_DEPTH_COMPONENT32F */, FormatDepth24 = 0x81A6 /* GL_DEPTH_COMPONENT24 */,
	FormatDepth16 = 0x81A5 /* GL_DEPTH_COMPONENT16 */;

/// A clock for the statistics.
using Clock = std::chrono::steady_clock;

/// Milliseconds between two moments.
double millisecondsBetween(Clock::time_point from, Clock::time_point to)
{
	return std::chrono::duration<double, std::milli>(to - from).count();
}

/// A vector turned by a unit quaternion: v + 2w (q x v) + 2 q x (q x v).
XrVector3f rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	XrVector3f out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

/// The inverse turn of a unit quaternion.
XrQuaternionf conjugate(const XrQuaternionf &q)
{
	XrQuaternionf out = { -q.x, -q.y, -q.z, q.w };
	return out;
}

/// The turn that first looks up by a pitch (about x) and then turns left by a yaw (about y), radians.
XrQuaternionf yawPitch(float yaw, float pitch)
{
	const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
	const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
	XrQuaternionf out = { cy * sp, sy * cp, -sy * sp, cy * cp };
	return out;
}

/// Where a pose looks: its yaw (radians, positive to the left of -z) and its pitch (positive up).
void lookOf(const XrQuaternionf &orientation, float &yaw, float &pitch)
{
	const XrVector3f ahead = { 0.0f, 0.0f, -1.0f };
	const XrVector3f forward = rotate(orientation, ahead);
	yaw = std::atan2(-forward.x, -forward.z);
	pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
}

/// Degrees to radians.
float radians(float degrees)
{
	return degrees * 3.14159265f / 180.0f;
}

/**
 * Where a controller's ray (from its aim pose, along its -z) meets a quad layer facing the player.
 * @param aim The controller's aim pose.
 * @param quad The quad's pose (its middle; it faces +z).
 * @param width The quad's width (metres).
 * @param height Its height.
 * @param across Set to how far across it the ray meets it, 0 to 1 from the left.
 * @param down Set to how far down, 0 to 1 from the top.
 * @return True when the ray meets the quad (in front of the controller, within its edges).
 */
bool rayOnQuad(const XrPosef &aim, const XrPosef &quad, float width, float height, float &across, float &down)
{
	const XrQuaternionf inverse = conjugate(quad.orientation);
	const XrVector3f relative = { aim.position.x - quad.position.x, aim.position.y - quad.position.y, aim.position.z - quad.position.z };
	const XrVector3f ahead = { 0.0f, 0.0f, -1.0f };
	const XrVector3f origin = rotate(inverse, relative);
	const XrVector3f along = rotate(inverse, rotate(aim.orientation, ahead));
	if (along.z > -1e-4f || width <= 0.0f || height <= 0.0f)
	{
		return false;
	}
	const float t = -origin.z / along.z;
	across = (origin.x + along.x * t) / width + 0.5f;
	down = 0.5f - (origin.y + along.y * t) / height;
	return t > 0.0f && across >= 0.0f && across <= 1.0f && down >= 0.0f && down <= 1.0f;
}

/// The HUD's panel, fixed where the head looks (the owner: "stationary to where we look (classic hud)"): its pose in the
/// head's own (VIEW) space, VrHudDistance straight ahead, facing the eyes (HudWidthDegrees wide: see Session::frame).
XrPosef hudPoseInView()
{
	XrPosef pose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -(float)VrHudDistance } };
	return pose;
}

/// A layer's swapchain: the runtime's images (textures of our context) for one picture.
struct Swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	int width = 0, height = 0;
	std::vector<XrSwapchainImageOpenGLESKHR> images;
	bool hasImage = false; // an image was released into it (only then may a layer show it)
};

/// A controller button and where it is (the Touch controllers' paths; see ButtonCount).
struct ButtonBinding
{
	Button button;
	const char *name, *description, *touchPath;
};
const ButtonBinding ButtonBindings[] =
{
	{ ButtonTriggerLeft, "trigger_left", "Left trigger", "/user/hand/left/input/trigger/value" },
	{ ButtonTriggerRight, "trigger_right", "Right trigger", "/user/hand/right/input/trigger/value" },
	{ ButtonGripLeft, "grip_left", "Left grip", "/user/hand/left/input/squeeze/value" },
	{ ButtonGripRight, "grip_right", "Right grip", "/user/hand/right/input/squeeze/value" },
	{ ButtonA, "button_a", "A", "/user/hand/right/input/a/click" },
	{ ButtonB, "button_b", "B", "/user/hand/right/input/b/click" },
	{ ButtonX, "button_x", "X", "/user/hand/left/input/x/click" },
	{ ButtonY, "button_y", "Y", "/user/hand/left/input/y/click" },
	{ ButtonMenu, "menu", "Menu", "/user/hand/left/input/menu/click" },
	{ ButtonLeftStick, "left_stick_click", "Left stick click", "/user/hand/left/input/thumbstick/click" },
	{ ButtonRightStick, "right_stick_click", "Right stick click", "/user/hand/right/input/thumbstick/click" },
};

/**
 * The OpenXR session and its frame loop (see the file's comment); one lives on the thread for
 * as long as the immersive activity does.
 */
class Session
{
public:
	/// A session for the activity, Khronos's loader given the application's context (global references, which the
	/// caller keeps alive for the game's life: the loader and the runtime may keep them).
	Session(JavaVM *vm, jobject activity, jobject applicationContext) : _vm(vm), _activity(activity), _applicationContext(applicationContext) {}
	/// The thread's whole life: start, the frames, the end.
	void run();

private:
	// ---- Starting ----
	/// Attaches the thread to Java (the loader and the runtime call Java on it) and names it.
	bool attachThread();
	/// Gives Khronos's loader the Java VM and the application's context (xrInitializeLoaderKHR), once in the game's life.
	bool initialiseLoader();
	/// Makes the instance (with the extensions we use, when the runtime has them) and finds the headset.
	bool createInstance();
	/// Makes our own OpenGL ES 3 context, current on this thread from now on.
	bool createGraphics();
	/// Makes the session on our context, and its spaces.
	bool createSession();
	/// Makes the controllers' actions, suggests where they are, and attaches them to the session.
	bool createActions();
	/// Picks the swapchains' colour format: sRGB, so the compositor shows our colours as they are.
	bool chooseFormat();

	// ---- Running ----
	/// The frames until the session ends or we are asked to leave.
	void loop();
	/// Handles OpenXR's events: the session's states, recentring, the controllers, losses.
	void pollEvents();
	/// One frame: wait, begin, read, upload, end.
	void frame();
	/// Reads the controllers for this frame and hands them over.
	void readControllers(XrTime time);
	/// Hands the controllers' state over to the game's thread, counting every press (the rays' hits come in state).
	void publishControllers(const bool held[ButtonCount], XrVector2f move, XrVector2f turn, const ControllerState &rays, bool focused);
	/// Finds the head and both eyes at this frame's display time and hands them over.
	void locateHead(XrTime time);
	/// Uploads whatever new pictures the game's thread handed over.
	void uploadPictures();
	/// Puts the monitor in front of where the head looks, level.
	void placeMonitor();
	/// Writes the statistics to the log every StatsSeconds.
	void reportStatistics();
#ifdef OPENXCOM3D_XR_FAKE_SESSION
	/// The emulator test build's stand-in for the whole session (see its comment).
	void fakeLoop();
	/// The emulator test build: an eye picture smoothed as the headset's would be, saved (see its comment).
	void saveSmoothedTestPicture(const LayerPicture &picture, const char *name);
#endif

	// ---- Helpers ----
	/// Logs a failed OpenXR call (its name and the result's); true when it succeeded.
	bool check(XrResult result, const char *what);
	/// Makes (or remakes at a new size) a swapchain: a colour one, or with depth true a depth one.
	bool makeSwapchain(Swapchain &chain, int width, int height, bool depth = false);
	/// Uploads a picture into a swapchain's next image.
	bool upload(Swapchain &chain, const LayerPicture &picture);
	/// Uploads an eye's depth (0 to 1, see DepthNearMetres) into a depth swapchain's next image.
	bool uploadDepth(Swapchain &chain, const std::vector<float> &depth);
	/// Takes a swapchain's next image, waits for it; false (and the swapchain gone) when that failed.
	bool acquireImage(Swapchain &chain, uint32_t &index);
	/// Ends everything: the swapchains, the actions, the spaces, the session, the instance, our context.
	void destroy();

	JavaVM *_vm;
	jobject _activity;
	jobject _applicationContext;
	bool _attached = false;

	XrInstance _instance = XR_NULL_HANDLE;
	XrSystemId _systemId = XR_NULL_SYSTEM_ID;
	XrSession _session = XR_NULL_HANDLE;
	XrSpace _localSpace = XR_NULL_HANDLE, _viewSpace = XR_NULL_HANDLE, _aimSpace = XR_NULL_HANDLE;
	XrSpace _rightGripSpace = XR_NULL_HANDLE, _leftAimSpace = XR_NULL_HANDLE; // (the right hand's aim is _aimSpace)
	XrSessionState _state = XR_SESSION_STATE_UNKNOWN;
	bool _running = false, _exit = false, _exitAsked = false;
	Clock::time_point _exitAskedAt;

	// Extensions the runtime has, and their functions.
	bool _hasPerformance = false, _hasThreadSettings = false, _hasRefreshRate = false, _hasDepthLayers = false;
	PFN_xrPerfSettingsSetPerformanceLevelEXT _setPerformanceLevel = nullptr;
	PFN_xrSetAndroidApplicationThreadKHR _setApplicationThread = nullptr;
	PFN_xrGetDisplayRefreshRateFB _getRefreshRate = nullptr;

	// Our OpenGL ES context.
	EGLDisplay _display = EGL_NO_DISPLAY;
	EGLConfig _config = nullptr;
	EGLContext _context = EGL_NO_CONTEXT;
	EGLSurface _surface = EGL_NO_SURFACE;
	int64_t _format = FormatSrgb;
	int64_t _depthFormat = 0;              // the eyes' depth's format (0: no depth layers)

	// The layers.
	Swapchain _eyeChains[2], _hudChain, _monitorChain;
	Swapchain _depthChains[2];             // the eyes' depth (XR_KHR_composition_layer_depth)
	bool _depthPairShown = false;          // both hold the depth of the eye pair shown
	VrEyeView _eyeViews[2] = {};
	bool _monitorPlaced = false;
	XrPosef _monitorPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
	XrPosef _head = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
	bool _headValid = false;
	XrVector3f _lastEyePositions[2] = {};  // the eyes' last valid places (used while the headset reports none)
	bool _eyePairShown = false;            // both eyes' swapchains hold the same pair (only then the projection layer)
	XrTime _recentreAt = 0;                // a recentring takes effect then: the quads are placed again after it
	XrExtent2Df _hudSize = { 0.0f, 0.0f }; // the HUD's panel's size (metres), as last shown...
	bool _hudShown = false;                // ...in the last frame (the right ray may point at it)
	uint64_t _frameNumber = 0;

	// The controllers.
	XrActionSet _actionSet = XR_NULL_HANDLE;
	XrAction _buttons[ButtonCount] = {};
	XrAction _move = XR_NULL_HANDLE, _turn = XR_NULL_HANDLE, _aim = XR_NULL_HANDLE;
	XrAction _rightGrip = XR_NULL_HANDLE, _leftAim = XR_NULL_HANDLE; // the right hand's grip pose, the left hand's aim pose
	bool _wasHeld[ButtonCount] = {};

	// The statistics (see reportStatistics).
	Clock::time_point _statsSince, _lastFrameEnd;
	int _statsFrames = 0, _statsNewEyes = 0, _statsUploads = 0;
	double _statsWaitMs = 0.0, _statsWaitWorstMs = 0.0, _statsGapWorstMs = 0.0, _statsUploadMs = 0.0;
	double _displayPeriodMs = 0.0;
	bool _glErrorLogged = false;

	// The eyes smoothed by the graphics chip (setting "VR smoothing", see smoothPair).
	EyeSmoother _smoother;
	bool _smootherFailed = false;          // it couldn't be made, or failed once: the eyes as they are from then on
	GLuint _smoothQueries[4] = {};         // GPU timers round each pair's smoothing (GL_EXT_disjoint_timer_query), in turn
	bool _smoothQueryBusy[4] = {};
	int _smoothQueryNext = 0;
	bool _hasTimerQuery = false;
	void (GL_APIENTRY *_beginQuery)(GLenum, GLuint) = nullptr;   // GL_EXT_disjoint_timer_query's
	void (GL_APIENTRY *_endQuery)(GLenum) = nullptr;
	int _statsSmoothed = 0, _statsSmoothTimed = 0;
	double _statsSmoothGpuMs = 0.0, _statsSmoothGpuWorstMs = 0.0;
	// The HUD held still in the room while the cursor is free (see frame).
	bool _hudHeldPlaced = false;
	XrPosef _hudHeldPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

	/// Makes the smoother on our context the first time it's wanted; false when there is none (the eyes as they are).
	bool smootherReady();
	/// Draws a new eye pair (and its depth) smoothed into the eye swapchains; false when it couldn't (none shown).
	bool smoothPair(const EyePictures &pictures, bool withDepth, bool &depthDone);
	/// Reads the GPU timers that have finished (the smoothing's time, for the statistics).
	void readSmoothingTimers();
};

/**
 * The thread's whole life: OpenXR started, the frames, everything ended; the shared block says
 * how it went (running, finished, failed).
 */
void Session::run()
{
	Shared &box = shared();
#ifdef OPENXCOM3D_XR_FAKE_SESSION
	const bool started = attachThread();
	if (started)
	{
		fakeLoop();
	}
#else
	const bool started = attachThread() && initialiseLoader() && createInstance() && createGraphics() && createSession() && createActions();
	if (started)
	{
		loop();
	}
#endif
	if (!started)
	{
		box.failed = true;
		log(ANDROID_LOG_ERROR, "the OpenXR session couldn't start (above: why); back to the pane");
	}
	destroy();
	box.running = false;
	if (_attached)
	{
		_vm->DetachCurrentThread();
	}
	log(ANDROID_LOG_INFO, "the OpenXR thread ends");
}

/**
 * Attaches this thread to Java (Khronos's loader asks Android for the runtime through Java, and
 * the runtime may call Java too) and names it for the system's tools.
 */
bool Session::attachThread()
{
	prctl(PR_SET_NAME, "OpenXcom3D XR", 0, 0, 0);
	JNIEnv *env = nullptr;
	if (_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
	{
		log(ANDROID_LOG_ERROR, "couldn't attach the OpenXR thread to Java");
		return false;
	}
	_attached = true;
	return true;
}

/**
 * Gives Khronos's loader the Java VM and the application's context: on Android it must have them
 * before the first instance (xrInitializeLoaderKHR, XR_KHR_loader_init_android). Once in the
 * game's life: the loader keeps what it was given (and hands it to the runtime), so it gets the
 * application's context, which lives as long as the game (an immersive activity is gone after its
 * trip into VR: the next trip's loader and runtime would hold a dead reference). (Only one
 * session thread runs at a time: see startSession.)
 */
bool Session::initialiseLoader()
{
	static bool initialised = false;
	if (initialised)
	{
		return true;
	}
	PFN_xrInitializeLoaderKHR initialize = nullptr;
	if (!check(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initialize), "xrGetInstanceProcAddr(xrInitializeLoaderKHR)") || !initialize)
	{
		return false;
	}
	XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = _vm;
	loaderInfo.applicationContext = _applicationContext;
	initialised = check(initialize((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo), "xrInitializeLoaderKHR");
	return initialised;
}

/**
 * Makes the instance: the two extensions we need (an Android instance, OpenGL ES), and three we
 * use when the runtime has them (the CPU level, the threads' roles, the display's rate); then
 * finds the headset and logs what it is.
 */
bool Session::createInstance()
{
	// What the runtime offers.
	uint32_t count = 0;
	if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
	{
		return false;
	}
	std::vector<XrExtensionProperties> available(count, { XR_TYPE_EXTENSION_PROPERTIES });
	check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()), "xrEnumerateInstanceExtensionProperties");
	auto has = [&](const char *name)
	{
		return std::any_of(available.begin(), available.end(), [&](const XrExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
	};
	std::vector<const char *> wanted = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
	for (const char *name : wanted)
	{
		if (!has(name))
		{
			log(ANDROID_LOG_ERROR, "the runtime lacks %s", name);
			return false;
		}
	}
	_hasPerformance = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	_hasThreadSettings = has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
	_hasRefreshRate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	_hasDepthLayers = has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	if (_hasDepthLayers) wanted.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	if (_hasPerformance) wanted.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (_hasThreadSettings) wanted.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
	if (_hasRefreshRate) wanted.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

	// The instance, made with the activity.
	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = _vm;
	androidInfo.applicationActivity = _activity;
	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.next = &androidInfo;
	createInfo.enabledExtensionCount = (uint32_t)wanted.size();
	createInfo.enabledExtensionNames = wanted.data();
	std::strncpy(createInfo.applicationInfo.applicationName, "OpenXcom3D", XR_MAX_APPLICATION_NAME_SIZE - 1);
	createInfo.applicationInfo.applicationVersion = 111;
	std::strncpy(createInfo.applicationInfo.engineName, "OpenXcom Extended", XR_MAX_ENGINE_NAME_SIZE - 1);
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	if (!check(xrCreateInstance(&createInfo, &_instance), "xrCreateInstance"))
	{
		return false;
	}
	XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	if (check(xrGetInstanceProperties(_instance, &instanceProperties), "xrGetInstanceProperties"))
	{
		log(ANDROID_LOG_INFO, "runtime: %s %u.%u.%u", instanceProperties.runtimeName,
			XR_VERSION_MAJOR(instanceProperties.runtimeVersion), XR_VERSION_MINOR(instanceProperties.runtimeVersion), XR_VERSION_PATCH(instanceProperties.runtimeVersion));
	}
	log(ANDROID_LOG_INFO, "extensions: CPU level %s, threads' roles %s, display rate %s, depth layers %s",
		_hasPerformance ? "yes" : "no", _hasThreadSettings ? "yes" : "no", _hasRefreshRate ? "yes" : "no", _hasDepthLayers ? "yes" : "no");

	// The extensions' functions.
	if (_hasPerformance) xrGetInstanceProcAddr(_instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&_setPerformanceLevel);
	if (_hasThreadSettings) xrGetInstanceProcAddr(_instance, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&_setApplicationThread);
	if (_hasRefreshRate) xrGetInstanceProcAddr(_instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&_getRefreshRate);

	// The headset.
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!check(xrGetSystem(_instance, &systemInfo, &_systemId), "xrGetSystem"))
	{
		return false;
	}
	XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
	if (check(xrGetSystemProperties(_instance, _systemId, &systemProperties), "xrGetSystemProperties"))
	{
		log(ANDROID_LOG_INFO, "headset: %s, layers up to %u, swapchains up to %ux%u", systemProperties.systemName,
			systemProperties.graphicsProperties.maxLayerCount, systemProperties.graphicsProperties.maxSwapchainImageWidth,
			systemProperties.graphicsProperties.maxSwapchainImageHeight);
	}
	uint32_t viewCount = 0;
	XrViewConfigurationView configViews[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	if (check(xrEnumerateViewConfigurationViews(_instance, _systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews), "xrEnumerateViewConfigurationViews") && viewCount == 2)
	{
		log(ANDROID_LOG_INFO, "the headset suggests %ux%u an eye (ours are the CPU's own size, see vrEyeSize)",
			configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight);
	}
	return true;
}

/**
 * Makes our own OpenGL ES 3 context, with a tiny off-screen surface, current on this thread from
 * now on: OpenXR's swapchain images are textures of it. (SDL's context, on the game's thread,
 * isn't touched; nothing is shared between the two.)
 */
bool Session::createGraphics()
{
	// What the runtime needs of OpenGL ES (it must be asked before the session).
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
	XrGraphicsRequirementsOpenGLESKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if (!check(xrGetInstanceProcAddr(_instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements), "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)")
		|| !check(getRequirements(_instance, _systemId, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR"))
	{
		return false;
	}

	// The display (SDL's too: never terminated here), a config for ES 3 with a pbuffer, the context, the surface.
	_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (_display == EGL_NO_DISPLAY || !eglInitialize(_display, &major, &minor))
	{
		log(ANDROID_LOG_ERROR, "EGL: no display (error 0x%x)", eglGetError());
		return false;
	}
	// The config: picked from the whole list, not by eglChooseConfig (which can hand back a
	// multisampled one, the developer setting "Force 4x MSAA" on): ES 3, exactly 8 bits a colour,
	// no multisampling, with off-screen (pbuffer) surfaces; one that has windows too if there is.
	EGLint configCount = 0;
	eglGetConfigs(_display, nullptr, 0, &configCount);
	std::vector<EGLConfig> configs(std::max(configCount, 0));
	eglGetConfigs(_display, configs.data(), configCount, &configCount);
	_config = nullptr;
	for (int pass = 0; pass < 2 && !_config; ++pass)
	{
		for (EGLConfig config : configs)
		{
			EGLint renderable = 0, surfaces = 0, red = 0, green = 0, blue = 0, alpha = 0, samples = 0;
			eglGetConfigAttrib(_display, config, EGL_RENDERABLE_TYPE, &renderable);
			eglGetConfigAttrib(_display, config, EGL_SURFACE_TYPE, &surfaces);
			eglGetConfigAttrib(_display, config, EGL_RED_SIZE, &red);
			eglGetConfigAttrib(_display, config, EGL_GREEN_SIZE, &green);
			eglGetConfigAttrib(_display, config, EGL_BLUE_SIZE, &blue);
			eglGetConfigAttrib(_display, config, EGL_ALPHA_SIZE, &alpha);
			eglGetConfigAttrib(_display, config, EGL_SAMPLES, &samples);
			const EGLint wantedSurfaces = pass == 0 ? (EGL_PBUFFER_BIT | EGL_WINDOW_BIT) : EGL_PBUFFER_BIT;
			if ((renderable & EGL_OPENGL_ES3_BIT_KHR) && (surfaces & wantedSurfaces) == wantedSurfaces
				&& red == 8 && green == 8 && blue == 8 && alpha == 8 && samples == 0)
			{
				_config = config;
				break;
			}
		}
	}
	if (!_config)
	{
		log(ANDROID_LOG_ERROR, "EGL: no ES 3 config with 8 bits a colour and no multisampling (of %d)", configCount);
		return false;
	}
	const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	_context = eglCreateContext(_display, _config, EGL_NO_CONTEXT, contextAttributes);
	const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	_surface = _context != EGL_NO_CONTEXT ? eglCreatePbufferSurface(_display, _config, surfaceAttributes) : EGL_NO_SURFACE;
	if (_surface == EGL_NO_SURFACE || !eglMakeCurrent(_display, _surface, _surface, _context))
	{
		log(ANDROID_LOG_ERROR, "EGL: no context of our own (error 0x%x)", eglGetError());
		return false;
	}
	log(ANDROID_LOG_INFO, "OpenGL ES: %s, %s (the runtime asks for %u.%u to %u.%u)", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
		XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
		XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));
	return true;
}

/**
 * Makes the session on our context, the swapchains' format, and the spaces: LOCAL (the room:
 * its origin where the head was at the last recentring, so the soldier's eye is the player's
 * eye whether seated or standing; y up) and VIEW (the head).
 */
bool Session::createSession()
{
	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = _display;
	binding.config = _config;
	binding.context = _context;
	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = _systemId;
	if (!check(xrCreateSession(_instance, &sessionInfo, &_session), "xrCreateSession") || !chooseFormat())
	{
		return false;
	}
	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!check(xrCreateReferenceSpace(_session, &spaceInfo, &_localSpace), "xrCreateReferenceSpace(LOCAL)"))
	{
		return false;
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	return check(xrCreateReferenceSpace(_session, &spaceInfo, &_viewSpace), "xrCreateReferenceSpace(VIEW)");
}

/**
 * Picks the swapchains' colour format: sRGB (the game's colours are sRGB values, and the
 * compositor then shows them as they are), or plain RGBA when the runtime has no sRGB one (the
 * colours then look paler: logged).
 */
bool Session::chooseFormat()
{
	uint32_t count = 0;
	if (!check(xrEnumerateSwapchainFormats(_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats") || count == 0)
	{
		return false;
	}
	std::vector<int64_t> formats(count);
	check(xrEnumerateSwapchainFormats(_session, count, &count, formats.data()), "xrEnumerateSwapchainFormats");
	if (std::find(formats.begin(), formats.end(), FormatSrgb) != formats.end())
	{
		_format = FormatSrgb;
	}
	else if (std::find(formats.begin(), formats.end(), FormatRgba) != formats.end())
	{
		_format = FormatRgba;
		log(ANDROID_LOG_WARN, "no sRGB swapchains: the colours will look paler");
	}
	else
	{
		_format = formats[0];
		log(ANDROID_LOG_WARN, "neither sRGB nor RGBA swapchains: format 0x%llx", (long long)_format);
	}
	log(ANDROID_LOG_INFO, "swapchain format 0x%llx (%u offered)", (long long)_format, count);

	// The eyes' depth (with XR_KHR_composition_layer_depth): the most precise depth format offered.
	_depthFormat = 0;
	if (_hasDepthLayers)
	{
		for (int64_t candidate : { FormatDepth32F, FormatDepth24, FormatDepth16 })
		{
			if (!_depthFormat && std::find(formats.begin(), formats.end(), candidate) != formats.end())
			{
				_depthFormat = candidate;
			}
		}
	}
	log(ANDROID_LOG_INFO, "the eyes' depth: %s (format 0x%llx)", _depthFormat ? "handed to the compositor" : "none", (long long)_depthFormat);
	return true;
}

/**
 * Makes the controllers' actions: a button each (the triggers and grips read as buttons, the
 * runtime deciding when a pull counts), the two sticks, and the right controller's aim (the ray
 * on the monitor); suggests where they are on the Touch controllers (the Quest 3's Touch Plus
 * take the same), with the simple controller as a fallback; and attaches them to the session.
 */
bool Session::createActions()
{
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	std::strcpy(setInfo.actionSetName, "openxcom3d");
	std::strcpy(setInfo.localizedActionSetName, "OpenXcom3D");
	if (!check(xrCreateActionSet(_instance, &setInfo, &_actionSet), "xrCreateActionSet"))
	{
		return false;
	}
	auto makeAction = [&](const char *name, const char *description, XrActionType type, XrAction &action)
	{
		XrActionCreateInfo actionInfo = { XR_TYPE_ACTION_CREATE_INFO };
		std::strncpy(actionInfo.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(actionInfo.localizedActionName, description, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		actionInfo.actionType = type;
		return check(xrCreateAction(_actionSet, &actionInfo, &action), "xrCreateAction");
	};
	for (const ButtonBinding &binding : ButtonBindings)
	{
		if (!makeAction(binding.name, binding.description, XR_ACTION_TYPE_BOOLEAN_INPUT, _buttons[binding.button]))
		{
			return false;
		}
	}
	if (!makeAction("move", "Walk", XR_ACTION_TYPE_VECTOR2F_INPUT, _move) || !makeAction("turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, _turn)
		|| !makeAction("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, _aim) || !makeAction("right_grip_pose", "Right hand", XR_ACTION_TYPE_POSE_INPUT, _rightGrip)
		|| !makeAction("left_aim", "Left aim", XR_ACTION_TYPE_POSE_INPUT, _leftAim))
	{
		return false;
	}

	// Where they are: the Touch controllers, then the simple controller (a fallback for other runtimes).
	auto path = [&](const char *text)
	{
		XrPath result = XR_NULL_PATH;
		check(xrStringToPath(_instance, text, &result), "xrStringToPath");
		return result;
	};
	auto suggest = [&](const char *profile, const std::vector<XrActionSuggestedBinding> &bindings)
	{
		XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggested.interactionProfile = path(profile);
		suggested.countSuggestedBindings = (uint32_t)bindings.size();
		suggested.suggestedBindings = bindings.data();
		return check(xrSuggestInteractionProfileBindings(_instance, &suggested), profile);
	};
	std::vector<XrActionSuggestedBinding> touch;
	for (const ButtonBinding &binding : ButtonBindings)
	{
		touch.push_back({ _buttons[binding.button], path(binding.touchPath) });
	}
	touch.push_back({ _move, path("/user/hand/left/input/thumbstick") });
	touch.push_back({ _turn, path("/user/hand/right/input/thumbstick") });
	touch.push_back({ _aim, path("/user/hand/right/input/aim/pose") });
	touch.push_back({ _rightGrip, path("/user/hand/right/input/grip/pose") });
	touch.push_back({ _leftAim, path("/user/hand/left/input/aim/pose") });
	const bool touchTaken = suggest("/interaction_profiles/oculus/touch_controller", touch);
	const std::vector<XrActionSuggestedBinding> simple =
	{
		{ _buttons[ButtonTriggerLeft], path("/user/hand/left/input/select/click") },
		{ _buttons[ButtonTriggerRight], path("/user/hand/right/input/select/click") },
		{ _buttons[ButtonMenu], path("/user/hand/left/input/menu/click") },
		{ _buttons[ButtonB], path("/user/hand/right/input/menu/click") },
		{ _aim, path("/user/hand/right/input/aim/pose") },
		{ _rightGrip, path("/user/hand/right/input/grip/pose") },
		{ _leftAim, path("/user/hand/left/input/aim/pose") },
	};
	const bool simpleTaken = suggest("/interaction_profiles/khr/simple_controller", simple);
	if (!touchTaken && !simpleTaken)
	{
		return false;
	}

	// Attached to the session, and the aim's space.
	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &_actionSet;
	if (!check(xrAttachSessionActionSets(_session, &attachInfo), "xrAttachSessionActionSets"))
	{
		return false;
	}
	XrActionSpaceCreateInfo aimInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	aimInfo.action = _aim;
	aimInfo.poseInActionSpace.orientation.w = 1.0f;
	if (!check(xrCreateActionSpace(_session, &aimInfo, &_aimSpace), "xrCreateActionSpace(aim)"))
	{
		return false;
	}
	// ...and the right hand's grip and the left hand's aim, for the weapon in hand (see locateHead).
	aimInfo.action = _rightGrip;
	check(xrCreateActionSpace(_session, &aimInfo, &_rightGripSpace), "xrCreateActionSpace(right grip)");
	aimInfo.action = _leftAim;
	check(xrCreateActionSpace(_session, &aimInfo, &_leftAimSpace), "xrCreateActionSpace(left aim)");
	return true;
}

/**
 * The frames, until the session ends or we are asked to leave (then the session is asked to end
 * first, and given ExitTimeoutMs to do it).
 */
void Session::loop()
{
	Shared &box = shared();
	_statsSince = _lastFrameEnd = Clock::now();
	while (!_exit)
	{
		pollEvents();
		if (box.stopRequested && !_exitAsked)
		{
			_exitAsked = true;
			_exitAskedAt = Clock::now();
			log(ANDROID_LOG_INFO, "leaving: the session is asked to end");
			if (_running)
			{
				check(xrRequestExitSession(_session), "xrRequestExitSession");
			}
			else
			{
				_exit = true;
			}
		}
		if (_exitAsked && millisecondsBetween(_exitAskedAt, Clock::now()) > ExitTimeoutMs)
		{
			log(ANDROID_LOG_WARN, "the session didn't end in %d ms: leaving anyway", ExitTimeoutMs);
			_exit = true;
		}
		if (_exit)
		{
			break;
		}
		if (!_running)
		{
			// Not running (the headset off, the session not yet ready): only the events, now and then.
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		frame();
	}
}

/**
 * Handles OpenXR's events: the session's states (begin it when ready, end it when stopping,
 * leave when exiting or lost), a recentring (the long press of the Meta button: the game
 * recentres too, and the monitor and the HUD come in front again), the controllers changing,
 * and lost events.
 */
void Session::pollEvents()
{
	Shared &box = shared();
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(_instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged &changed = *(const XrEventDataSessionStateChanged *)&event;
			static const char *const names[] = { "unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping", "loss pending", "exiting" };
			_state = changed.state;
			log(ANDROID_LOG_INFO, "session: %s", (unsigned)_state < 9 ? names[_state] : "?");
			if (_state == XR_SESSION_STATE_READY && !_exitAsked)
			{
				// Begin: the frames start; the CPU at its highest steady level (our 3D view is drawn by it),
				// the GPU at its lowest (we only upload pictures); our two threads' roles for the scheduler.
				XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (check(xrBeginSession(_session, &beginInfo), "xrBeginSession"))
				{
					_running = true;
					box.running = true;
					if (_setPerformanceLevel)
					{
						check(_setPerformanceLevel(_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT), "CPU level");
						check(_setPerformanceLevel(_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT), "GPU level");
					}
					if (_setApplicationThread)
					{
						check(_setApplicationThread(_session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid()), "the OpenXR thread's role");
						if (box.gameThreadId)
						{
							check(_setApplicationThread(_session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, (uint32_t)box.gameThreadId), "the game thread's role");
						}
					}
					float rate = 0.0f;
					if (_getRefreshRate && check(_getRefreshRate(_session, &rate), "xrGetDisplayRefreshRateFB"))
					{
						log(ANDROID_LOG_INFO, "display: %.0f Hz", rate);
					}
				}
			}
			else if (_state == XR_SESSION_STATE_STOPPING)
			{
				check(xrEndSession(_session), "xrEndSession");
				_running = false;
				box.running = false;
				if (_exitAsked)
				{
					_exit = true;
				}
			}
			else if (_state == XR_SESSION_STATE_EXITING || _state == XR_SESSION_STATE_LOSS_PENDING)
			{
				_exit = true;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			log(ANDROID_LOG_WARN, "the runtime is going away");
			_exit = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
		{
			const XrEventDataReferenceSpaceChangePending &change = *(const XrEventDataReferenceSpaceChangePending *)&event;
			if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				// (The game recentres at once; the monitor and the HUD come in front again once the change has
				// taken effect, from the head as it is then.)
				log(ANDROID_LOG_INFO, "recentred by the headset");
				++box.recentres;
				_recentreAt = std::max<XrTime>(change.changeTime, 1);
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
		{
			// Which controllers each hand has now (the input log: tag OpenXcom3DInput).
			for (const char *hand : { "/user/hand/left", "/user/hand/right" })
			{
				XrPath handPath = XR_NULL_PATH;
				XrInteractionProfileState profile = { XR_TYPE_INTERACTION_PROFILE_STATE };
				if (xrStringToPath(_instance, hand, &handPath) == XR_SUCCESS && xrGetCurrentInteractionProfile(_session, handPath, &profile) == XR_SUCCESS)
				{
					char name[XR_MAX_PATH_LENGTH] = "none";
					uint32_t length = 0;
					if (profile.interactionProfile != XR_NULL_PATH)
					{
						xrPathToString(_instance, profile.interactionProfile, sizeof(name), &length, name);
					}
					__android_log_print(ANDROID_LOG_INFO, "OpenXcom3DInput", "VR controller %s: %s", hand, name);
					logToGame(std::string("OpenXcom3DInput: VR controller ") + hand + ": " + name);
				}
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_EVENTS_LOST:
			log(ANDROID_LOG_WARN, "%u OpenXR events lost", ((const XrEventDataEventsLost *)&event)->lostEventCount);
			break;
		default:
			break;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

/**
 * One frame at the headset's rate: wait for it, begin it, read the controllers and the head for
 * its display time, upload any new pictures, and end it with the layers (back to front: the
 * eyes, the monitor, the HUD).
 */
void Session::frame()
{
	Shared &box = shared();
	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };
	const Clock::time_point waitStart = Clock::now();
	if (!check(xrWaitFrame(_session, &waitInfo, &frameState), "xrWaitFrame"))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return;
	}
	const Clock::time_point waited = Clock::now();
	const double waitMs = millisecondsBetween(waitStart, waited);
	_statsWaitMs += waitMs;
	_statsWaitWorstMs = std::max(_statsWaitWorstMs, waitMs);
	_displayPeriodMs = frameState.predictedDisplayPeriod / 1.0e6;
	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if (!check(xrBeginFrame(_session, &beginInfo), "xrBeginFrame"))
	{
		return;
	}
	++_frameNumber;

	// What the game needs (the controllers, the head), and what it made (the pictures).
	readControllers(frameState.predictedDisplayTime);
	locateHead(frameState.predictedDisplayTime);
	uploadPictures();
	if (box.placeQuads.exchange(false) || (_recentreAt && frameState.predictedDisplayTime >= _recentreAt))
	{
		_monitorPlaced = false;
		_recentreAt = 0;
	}

	// The layers.
	std::vector<const XrCompositionLayerBaseHeader *> layers;
	XrCompositionLayerProjectionView projectionViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerDepthInfoKHR depthInfos[2] = { { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR }, { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR } };
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad monitor = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad hud = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	if (frameState.shouldRender)
	{
		// The eyes: the newest pictures, each with the pose it was drawn for and the field of view it shows (the
		// compositor turns them to where the head is now).
		if (_eyePairShown && _eyeChains[0].hasImage && _eyeChains[1].hasImage)
		{
			for (int eye = 0; eye < 2; ++eye)
			{
				const VrEyeView &view = _eyeViews[eye];
				projectionViews[eye].pose.position = { view.pose.position[0], view.pose.position[1], view.pose.position[2] };
				projectionViews[eye].pose.orientation = { view.pose.orientation[0], view.pose.orientation[1], view.pose.orientation[2], view.pose.orientation[3] };
				projectionViews[eye].fov = { view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown };
				projectionViews[eye].subImage.swapchain = _eyeChains[eye].handle;
				projectionViews[eye].subImage.imageRect = { { 0, 0 }, { _eyeChains[eye].width, _eyeChains[eye].height } };
				projectionViews[eye].subImage.imageArrayIndex = 0;
				// The eye's depth, when it came with the pair: the compositor can then move near things with the head
				// between our frames (the weapon in hand, fixed to the head, may judder a little on fast turns then).
				if (_depthPairShown && _depthChains[eye].hasImage)
				{
					depthInfos[eye].subImage.swapchain = _depthChains[eye].handle;
					depthInfos[eye].subImage.imageRect = { { 0, 0 }, { _depthChains[eye].width, _depthChains[eye].height } };
					depthInfos[eye].subImage.imageArrayIndex = 0;
					depthInfos[eye].minDepth = 0.0f;
					depthInfos[eye].maxDepth = 1.0f;
					depthInfos[eye].nearZ = DepthNearMetres;
					depthInfos[eye].farZ = DepthFarMetres;
					projectionViews[eye].next = &depthInfos[eye];
				}
			}
			projection.space = _localSpace;
			projection.viewCount = 2;
			projection.views = projectionViews;
			layers.push_back((const XrCompositionLayerBaseHeader *)&projection);
		}
		// The monitor: placed in front of the head when it comes up, then fixed in the room.
		const bool monitorShown = box.showMonitor && _monitorChain.hasImage && _headValid;
		if (monitorShown)
		{
			if (!_monitorPlaced)
			{
				placeMonitor();
			}
			monitor.layerFlags = 0;
			monitor.space = _localSpace;
			monitor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			monitor.subImage.swapchain = _monitorChain.handle;
			monitor.subImage.imageRect = { { 0, 0 }, { _monitorChain.width, _monitorChain.height } };
			monitor.pose = _monitorPose;
			monitor.size = { MonitorWidth, MonitorWidth * _monitorChain.height / std::max(1, _monitorChain.width) };
			layers.push_back((const XrCompositionLayerBaseHeader *)&monitor);
		}
		else
		{
			_monitorPlaced = false;
		}
		// The HUD: transparent where empty, HudWidthDegrees wide at VrHudDistance, fixed where the head looks (a quad
		// in the VIEW space: the compositor keeps it there with the head's newest pose, a classic HUD); while the cursor
		// is free it holds still in the room where it was (placed where the head looked then), so the head can turn to
		// any of its buttons for the right ray; mouse look back, it is fixed where the head looks again at once.
		const bool hudShown = box.showHud && _hudChain.hasImage;
		if (hudShown)
		{
			const float width = 2.0f * (float)VrHudDistance * std::tan(radians(HudWidthDegrees) * 0.5f);
			hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			hud.subImage.swapchain = _hudChain.handle;
			hud.subImage.imageRect = { { 0, 0 }, { _hudChain.width, _hudChain.height } };
			if (box.hudHeld && _headValid)
			{
				if (!_hudHeldPlaced)
				{
					const XrPosef inView = hudPoseInView();
					const XrVector3f offset = rotate(_head.orientation, inView.position);
					_hudHeldPose.orientation = _head.orientation;
					_hudHeldPose.position = { _head.position.x + offset.x, _head.position.y + offset.y, _head.position.z + offset.z };
					_hudHeldPlaced = true;
				}
				hud.space = _localSpace;
				hud.pose = _hudHeldPose;
			}
			else
			{
				_hudHeldPlaced = false;
				hud.space = _viewSpace;
				hud.pose = hudPoseInView();
			}
			hud.size = { width, width * _hudChain.height / std::max(1, _hudChain.width) };
			layers.push_back((const XrCompositionLayerBaseHeader *)&hud);
			_hudSize = hud.size;
		}
		else
		{
			_hudHeldPlaced = false;
		}
		_hudShown = hudShown;
	}
	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (uint32_t)layers.size();
	endInfo.layers = layers.data();
	check(xrEndFrame(_session, &endInfo), "xrEndFrame");

	// The statistics.
	const Clock::time_point ended = Clock::now();
	_statsGapWorstMs = std::max(_statsGapWorstMs, millisecondsBetween(_lastFrameEnd, ended));
	_lastFrameEnd = ended;
	++_statsFrames;
	reportStatistics();
}

/**
 * Reads the controllers for this frame (only while the session has the input: under the
 * system's menu everything reads released), finds where the right controller's ray meets the
 * monitor, and hands it all over, counting every press so none is lost between the game's
 * slower frames.
 * @param time The frame's display time.
 */
void Session::readControllers(XrTime time)
{
	Shared &box = shared();
	const bool focused = _state == XR_SESSION_STATE_FOCUSED;
	bool held[ButtonCount] = {};
	XrVector2f move = { 0.0f, 0.0f }, turn = { 0.0f, 0.0f };
	ControllerState rays;
	if (focused)
	{
		XrActiveActionSet active = { _actionSet, XR_NULL_PATH };
		XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
		syncInfo.countActiveActionSets = 1;
		syncInfo.activeActionSets = &active;
		check(xrSyncActions(_session, &syncInfo), "xrSyncActions");
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		for (int button = 0; button < ButtonCount; ++button)
		{
			XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
			getInfo.action = _buttons[button];
			if (xrGetActionStateBoolean(_session, &getInfo, &state) == XR_SUCCESS && state.isActive)
			{
				held[button] = state.currentState;
			}
		}
		XrActionStateVector2f stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
		getInfo.action = _move;
		if (xrGetActionStateVector2f(_session, &getInfo, &stick) == XR_SUCCESS && stick.isActive)
		{
			move = stick.currentState;
		}
		getInfo.action = _turn;
		stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
		if (xrGetActionStateVector2f(_session, &getInfo, &stick) == XR_SUCCESS && stick.isActive)
		{
			turn = stick.currentState;
		}

		// The right controller's ray on the monitor (in the room) and on the HUD's panel (fixed where the head looks: the
		// ray found in the head's own space), as the last frame showed them.
		const XrSpaceLocationFlags poseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		XrSpaceLocation aim = { XR_TYPE_SPACE_LOCATION };
		if (_monitorPlaced && xrLocateSpace(_aimSpace, _localSpace, time, &aim) == XR_SUCCESS && (aim.locationFlags & poseValid) == poseValid)
		{
			const float height = MonitorWidth * _monitorChain.height / std::max(1, _monitorChain.width);
			rays.rayOnMonitor = rayOnQuad(aim.pose, _monitorPose, MonitorWidth, height, rays.monitorX, rays.monitorY);
		}
		XrSpaceLocation aimForHud = { XR_TYPE_SPACE_LOCATION };
		if (_hudShown && xrLocateSpace(_aimSpace, _hudHeldPlaced ? _localSpace : _viewSpace, time, &aimForHud) == XR_SUCCESS
			&& (aimForHud.locationFlags & poseValid) == poseValid)
		{
			// (held still in the room: the ray in the room's space; else in the head's own)
			rays.rayOnHud = rayOnQuad(aimForHud.pose, _hudHeldPlaced ? _hudHeldPose : hudPoseInView(), _hudSize.width, _hudSize.height, rays.hudX, rays.hudY);
		}
	}
	publishControllers(held, move, turn, rays, focused);
}

/**
 * Hands the controllers over to the game's thread: the state now, and the presses since the game
 * last took them (none lost however short).
 */
void Session::publishControllers(const bool held[ButtonCount], XrVector2f move, XrVector2f turn, const ControllerState &rays, bool focused)
{
	Shared &box = shared();
	std::lock_guard<std::mutex> lock(box.controllerMutex);
	ControllerState &out = box.controllers;
	for (int button = 0; button < ButtonCount; ++button)
	{
		if (held[button] && !_wasHeld[button])
		{
			++out.presses[button];
		}
		out.held[button] = held[button];
		_wasHeld[button] = held[button];
	}
	out.move[0] = move.x;
	out.move[1] = move.y;
	out.turn[0] = turn.x;
	out.turn[1] = turn.y;
	out.rayOnMonitor = rays.rayOnMonitor;
	out.monitorX = rays.monitorX;
	out.monitorY = rays.monitorY;
	out.rayOnHud = rays.rayOnHud;
	out.hudX = rays.hudX;
	out.hudY = rays.hudY;
	out.focused = focused;
}

/**
 * Finds the head (the VIEW space) and both eyes (xrLocateViews) in the LOCAL space at this
 * frame's display time, and hands them over for the game's next pictures.
 * @param time The frame's display time.
 */
void Session::locateHead(XrTime time)
{
	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = _localSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t viewCount = 0;
	XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
	const bool viewsFound = check(xrLocateViews(_session, &locateInfo, &viewState, 2, &viewCount, views), "xrLocateViews") && viewCount == 2
		&& (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
	const bool headFound = check(xrLocateSpace(_viewSpace, _localSpace, time, &head), "xrLocateSpace(VIEW)")
		&& (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
	if (!viewsFound || !headFound)
	{
		return;
	}
	// Places the headset doesn't vouch for (tracking lost: only the turn is known) aren't read: the last valid
	// ones stand in (OpenXR: a pose's position is undefined without its valid bit).
	if (!(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
	{
		head.pose.position = _head.position;
	}
	for (int eye = 0; eye < 2; ++eye)
	{
		if (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
		{
			_lastEyePositions[eye] = views[eye].pose.position;
		}
		else
		{
			views[eye].pose.position = _lastEyePositions[eye];
		}
	}
	_head = head.pose;
	_headValid = true;

	HeadSample sample;
	sample.valid = true;
	sample.frame = _frameNumber;
	sample.head.position[0] = head.pose.position.x;
	sample.head.position[1] = head.pose.position.y;
	sample.head.position[2] = head.pose.position.z;
	sample.head.orientation[0] = head.pose.orientation.x;
	sample.head.orientation[1] = head.pose.orientation.y;
	sample.head.orientation[2] = head.pose.orientation.z;
	sample.head.orientation[3] = head.pose.orientation.w;
	for (int eye = 0; eye < 2; ++eye)
	{
		VrEyeView &view = sample.views[eye];
		view.pose.position[0] = views[eye].pose.position.x;
		view.pose.position[1] = views[eye].pose.position.y;
		view.pose.position[2] = views[eye].pose.position.z;
		view.pose.orientation[0] = views[eye].pose.orientation.x;
		view.pose.orientation[1] = views[eye].pose.orientation.y;
		view.pose.orientation[2] = views[eye].pose.orientation.z;
		view.pose.orientation[3] = views[eye].pose.orientation.w;
		view.fov.angleLeft = views[eye].fov.angleLeft;
		view.fov.angleRight = views[eye].fov.angleRight;
		view.fov.angleUp = views[eye].fov.angleUp;
		view.fov.angleDown = views[eye].fov.angleDown;
	}
	// The controllers at the same time, in the same space (the weapon in the right hand, see vrSetHands): a pose counts
	// only with both its position and its turn tracked.
	const XrSpace handSpaces[HandPoseCount] = { _aimSpace, _rightGripSpace, _leftAimSpace };
	for (int hand = 0; hand < HandPoseCount; ++hand)
	{
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
		const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if (handSpaces[hand] != XR_NULL_HANDLE && xrLocateSpace(handSpaces[hand], _localSpace, time, &location) == XR_SUCCESS
			&& (location.locationFlags & tracked) == tracked)
		{
			sample.handTracked[hand] = true;
			sample.hands[hand] = { { location.pose.position.x, location.pose.position.y, location.pose.position.z },
				{ location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w } };
		}
	}
	Shared &box = shared();
	std::lock_guard<std::mutex> lock(box.sampleMutex);
	box.sample = sample;
}

/**
 * Uploads whatever new pictures the game's thread handed over since the last frame: both eyes
 * (with the views they were drawn for), the HUD, the monitor. A swapchain is made, or remade,
 * at its picture's size.
 */
void Session::uploadPictures()
{
	Shared &box = shared();
	readSmoothingTimers();
	const Clock::time_point start = Clock::now();
	bool uploaded = false;
	if (box.eyes.take())
	{
		const EyePictures &pictures = box.eyes.front();
		bool both = true;
		bool depthBoth = _depthFormat && !pictures.depth[0].empty() && !pictures.depth[1].empty();
		if (box.smoothing && smootherReady())
		{
			// Smoothed by the graphics chip, bigger, the depth with them (see smoothPair).
			bool depthDone = false;
			both = smoothPair(pictures, depthBoth, depthDone);
			depthBoth = both && depthDone;
		}
		else
		{
			// As they are.
			for (int eye = 0; eye < 2; ++eye)
			{
				both = both && makeSwapchain(_eyeChains[eye], pictures.eyes[eye].width, pictures.eyes[eye].height) && upload(_eyeChains[eye], pictures.eyes[eye]);
			}
			// Their depth, with them (none: the projection layer without depth).
			depthBoth = depthBoth && both;
			for (int eye = 0; eye < 2 && depthBoth; ++eye)
			{
				depthBoth = makeSwapchain(_depthChains[eye], pictures.eyes[eye].width, pictures.eyes[eye].height, true)
					&& uploadDepth(_depthChains[eye], pictures.depth[eye]);
			}
		}
		// (Only a pair whose both pictures went in is shown: one eye new and the other old would be a broken pair.)
		_eyePairShown = both;
		_depthPairShown = depthBoth;
		if (both)
		{
			_eyeViews[0] = pictures.views[0];
			_eyeViews[1] = pictures.views[1];
			++_statsNewEyes;
		}
		uploaded = true;
	}
	if (box.hud.take())
	{
		const LayerPicture &picture = box.hud.front();
		if (makeSwapchain(_hudChain, picture.width, picture.height))
		{
			upload(_hudChain, picture);
		}
		uploaded = true;
	}
	if (box.monitor.take())
	{
		const LayerPicture &picture = box.monitor.front();
		if (makeSwapchain(_monitorChain, picture.width, picture.height))
		{
			upload(_monitorChain, picture);
		}
		uploaded = true;
	}
	if (uploaded)
	{
		_statsUploadMs += millisecondsBetween(start, Clock::now());
		++_statsUploads;
	}
}

/**
 * Puts the monitor in front of where the head looks now: level, MonitorDistance ahead, its
 * middle MonitorDrop below the eyes, facing the player.
 */
void Session::placeMonitor()
{
	float yaw, pitch;
	lookOf(_head.orientation, yaw, pitch);
	_monitorPose.orientation = yawPitch(yaw, 0.0f);
	const XrVector3f ahead = { 0.0f, -MonitorDrop, -MonitorDistance };
	const XrVector3f offset = rotate(_monitorPose.orientation, ahead);
	_monitorPose.position = { _head.position.x + offset.x, _head.position.y + offset.y, _head.position.z + offset.z };
	_monitorPlaced = true;
}

/**
 * Every StatsSeconds, writes the frames to the log: how many went to the headset and at what
 * rate (and the display's period), how long each waited for its turn (xrWaitFrame), the worst
 * gap between two, how many new eye pictures came (the rest showed an older one again, turned
 * with the head) and the uploads' time.
 */
void Session::reportStatistics()
{
	const Clock::time_point now = Clock::now();
	const double elapsedMs = millisecondsBetween(_statsSince, now);
	if (elapsedMs < StatsSeconds * 1000.0 || _statsFrames == 0)
	{
		return;
	}
	const double seconds = elapsedMs / 1000.0;
	log(ANDROID_LOG_INFO, "frames: %d in %.1f s (%.1f a second, the display's period %.1f ms), waiting for each %.1f ms on average, %.1f ms the most, "
		"the worst gap %.1f ms; new eye pictures %d (%.1f a second, the other frames showed the last again); uploads %.2f ms on average",
		_statsFrames, seconds, _statsFrames / seconds, _displayPeriodMs, _statsWaitMs / _statsFrames, _statsWaitWorstMs,
		_statsGapWorstMs, _statsNewEyes, _statsNewEyes / seconds, _statsUploads ? _statsUploadMs / _statsUploads : 0.0);
	if (_statsSmoothed > 0)
	{
		log(ANDROID_LOG_INFO, "smoothing (%s): %d pairs, the GPU %.2f ms a pair on average, %.2f ms the most (%d timed)",
			EyeSmoothingFilter == SmoothingFilter::Sabr ? "SABR" : "FSR 1", _statsSmoothed, _statsSmoothTimed ? _statsSmoothGpuMs / _statsSmoothTimed : 0.0,
			_statsSmoothGpuWorstMs, _statsSmoothTimed);
	}
	_statsSmoothed = _statsSmoothTimed = 0;
	_statsSmoothGpuMs = _statsSmoothGpuWorstMs = 0.0;
	_statsSince = now;
	_statsFrames = _statsNewEyes = _statsUploads = 0;
	_statsWaitMs = _statsWaitWorstMs = _statsGapWorstMs = _statsUploadMs = 0.0;
}

#ifdef OPENXCOM3D_XR_FAKE_SESSION
/**
 * THE EMULATOR TEST BUILD ONLY: saves a layer's picture (RGBA, the bottom row first) as a 32-bit
 * BMP in the game's profile folder (/sdcard/openxcom3d), whose rows run bottom first too: the
 * file shows the picture upright exactly when the picture was handed over the right way up.
 * @param picture The picture.
 * @param name The file's name.
 */
void saveTestPicture(const LayerPicture &picture, const char *name)
{
	const std::string path = std::string("/sdcard/openxcom3d/") + name;
	FILE *file = std::fopen(path.c_str(), "wb");
	if (!file || picture.width <= 0 || picture.height <= 0)
	{
		if (file) std::fclose(file);
		return;
	}
	const uint32_t imageBytes = (uint32_t)picture.width * picture.height * 4;
	uint8_t header[54] = { 'B', 'M' };
	auto put32 = [&](int at, uint32_t value) { for (int i = 0; i < 4; ++i) header[at + i] = (uint8_t)(value >> (8 * i)); };
	put32(2, 54 + imageBytes);
	put32(10, 54);
	put32(14, 40);
	put32(18, (uint32_t)picture.width);
	put32(22, (uint32_t)picture.height);
	header[26] = 1;
	header[28] = 32;
	put32(34, imageBytes);
	std::fwrite(header, 1, sizeof(header), file);
	// RGBA (R first in memory) into BMP's BGRA, row by row as they are (both bottom first).
	std::vector<uint8_t> row((size_t)picture.width * 4);
	for (int y = 0; y < picture.height; ++y)
	{
		const Uint32 *from = picture.pixels.data() + (size_t)y * picture.width;
		for (int x = 0; x < picture.width; ++x)
		{
			const Uint32 pixel = from[x];
			row[4 * x + 0] = (uint8_t)(pixel >> 16);
			row[4 * x + 1] = (uint8_t)(pixel >> 8);
			row[4 * x + 2] = (uint8_t)pixel;
			row[4 * x + 3] = (uint8_t)(pixel >> 24);
		}
		std::fwrite(row.data(), 1, row.size(), file);
	}
	std::fclose(file);
	log(ANDROID_LOG_INFO, "TEST BUILD: saved %s (%dx%d)", path.c_str(), picture.width, picture.height);
}

/**
 * THE EMULATOR TEST BUILD ONLY: an eye picture smoothed by the emulator's graphics as the headset's would be (the same
 * filter, into an sRGB texture as an eye's swapchain image is), read back and saved as a BMP; and the time it took.
 * Our own context is made for it (no OpenXR here), as createGraphics makes it.
 * @param picture The eye picture.
 * @param name The file's name.
 */
void Session::saveSmoothedTestPicture(const LayerPicture &picture, const char *name)
{
	if (_context == EGL_NO_CONTEXT)
	{
		_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		EGLint major = 0, minor = 0, count = 0;
		const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
		const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
		if (!eglInitialize(_display, &major, &minor) || !eglChooseConfig(_display, configAttributes, &_config, 1, &count) || count < 1)
		{
			log(ANDROID_LOG_ERROR, "TEST BUILD: no EGL config for the smoothing's test");
			return;
		}
		_context = eglCreateContext(_display, _config, EGL_NO_CONTEXT, contextAttributes);
		_surface = _context != EGL_NO_CONTEXT ? eglCreatePbufferSurface(_display, _config, surfaceAttributes) : EGL_NO_SURFACE;
		if (_surface == EGL_NO_SURFACE || !eglMakeCurrent(_display, _surface, _surface, _context))
		{
			log(ANDROID_LOG_ERROR, "TEST BUILD: no context for the smoothing's test");
			return;
		}
	}
	if (!smootherReady())
	{
		return;
	}
	const int scale = smoothingScaleFor(picture.width, picture.height);
	LayerPicture smoothed;
	smoothed.width = picture.width * scale;
	smoothed.height = picture.height * scale;
	smoothed.pixels.resize((size_t)smoothed.width * smoothed.height);
	GLuint target = 0, framebuffer = 0;
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, smoothed.width, smoothed.height);
	glBindTexture(GL_TEXTURE_2D, 0);
	const Clock::time_point start = Clock::now();
	const bool drawn = _smoother.drawColour(picture.pixels.data(), picture.width, picture.height, target, smoothed.width, smoothed.height, true);
	glFinish();
	const double ms = millisecondsBetween(start, Clock::now());
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	glReadPixels(0, 0, smoothed.width, smoothed.height, GL_RGBA, GL_UNSIGNED_BYTE, smoothed.pixels.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteTextures(1, &target);
	log(ANDROID_LOG_INFO, "TEST BUILD: an eye smoothed (%s): %dx%d into %dx%d, %s, %.1f ms on the emulator's graphics",
		EyeSmoothingFilter == SmoothingFilter::Sabr ? "SABR" : "FSR 1", picture.width, picture.height, smoothed.width, smoothed.height, drawn ? "drawn" : "FAILED", ms);
	if (drawn)
	{
		saveTestPicture(smoothed, name);
	}
}

/**
 * THE EMULATOR TEST BUILD ONLY (-DOPENXCOM3D_XR_FAKE_SESSION; never in a build for the headset):
 * an emulator has no OpenXR runtime, so the thread acts out a running session instead, at 72
 * frames a second: a head turning slowly from side to side, a Quest 3's two eyes (63 mm apart,
 * its fields of view), the controllers idle, and every picture the game hands over taken and
 * counted (the log's statistics as the real loop's). So the game's side of VR mode and the
 * hand-over between the pane and the immersive activity can be tried without a headset.
 */
void Session::fakeLoop()
{
	Shared &box = shared();
	log(ANDROID_LOG_WARN, "TEST BUILD: a session acted out, no OpenXR");
	box.running = true;
	const Clock::time_point start = Clock::now();
	_statsSince = _lastFrameEnd = start;
	_displayPeriodMs = 1000.0 / 72.0;
	// A Quest 3's left eye: 54 degrees left, 40 right, 44 up, 55 down (the right eye mirrored).
	const float fov[4] = { -0.9425f, 0.6981f, 0.7679f, -0.9599f };
	bool sizesLogged = false;
	// The acted-out sessions so far: the first one ends the turn (the aliens' turn takes VR back to the pane, the
	// next turn brings it back by itself), the later ones leave with B.
	static int sessionsActed = 0;
	const bool endsTheTurn = sessionsActed++ == 0;
	// Or the headset's sequence (the owner's report on build 114; /sdcard/openxcom3d/vrtest_script.txt says "headset"):
	// every session A at 3 (the inventory), A at 6 (closed), B at 9 (back to the pane), the head moving about as a real
	// one does (half a metre side to side, a little forward and down), its eyes with it.
	bool headsetScript = false;
	if (FILE *script = std::fopen("/sdcard/openxcom3d/vrtest_script.txt", "r"))
	{
		char word[32] = "";
		headsetScript = std::fscanf(script, "%31s", word) == 1 && std::strcmp(word, "headset") == 0;
		std::fclose(script);
	}
	log(ANDROID_LOG_INFO, "TEST BUILD: the script: %s", headsetScript ? "the headset's (A, A, B)" : (endsTheTurn ? "Y ends the turn" : "B at 18 s"));
	bool smoothedSaved = false, hudHeldWas = false;
	while (!box.stopRequested)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(13889));
		const float seconds = (float)(millisecondsBetween(start, Clock::now()) / 1000.0);
		++_frameNumber;

		// The head: turning 30 degrees either way every 12 seconds, looking a little down.
		const XrQuaternionf headTurn = yawPitch(radians(30.0f) * std::sin(seconds * 0.5236f), radians(-5.0f));
		HeadSample sample;
		sample.valid = true;
		sample.frame = _frameNumber;
		sample.head.orientation[0] = headTurn.x;
		sample.head.orientation[1] = headTurn.y;
		sample.head.orientation[2] = headTurn.z;
		sample.head.orientation[3] = headTurn.w;
		const XrVector3f right = rotate(headTurn, { 0.0315f, 0.0f, 0.0f });
		// (the headset's script: the head itself moving about, as the owner's does)
		const XrVector3f headPlace = headsetScript ? XrVector3f{ 0.25f * std::sin(seconds * 0.9f), -0.05f - 0.05f * std::sin(seconds * 0.4f), -0.1f * std::sin(seconds * 0.6f) }
			: XrVector3f{ 0.0f, 0.0f, 0.0f };
		for (int axis = 0; axis < 3; ++axis)
		{
			sample.head.position[axis] = axis == 0 ? headPlace.x : axis == 1 ? headPlace.y : headPlace.z;
		}
		for (int eye = 0; eye < 2; ++eye)
		{
			VrEyeView &view = sample.views[eye];
			const float side = eye == 0 ? -1.0f : 1.0f;
			view.pose.position[0] = headPlace.x + right.x * side;
			view.pose.position[1] = headPlace.y + right.y * side;
			view.pose.position[2] = headPlace.z + right.z * side;
			for (int i = 0; i < 4; ++i)
			{
				view.pose.orientation[i] = sample.head.orientation[i];
			}
			view.fov.angleLeft = eye == 0 ? fov[0] : -fov[1];
			view.fov.angleRight = eye == 0 ? fov[1] : -fov[0];
			view.fov.angleUp = fov[2];
			view.fov.angleDown = fov[3];
		}
		// The controllers (the weapon in the right hand): the right one held at the chest's right, pointing where the
		// head looks, a little down; its grip a hand's width back; the left one ahead of it on the left.
		auto handAt = [&](XrVector3f offset, int hand)
		{
			const XrVector3f place = rotate(headTurn, offset);
			sample.hands[hand] = { { headPlace.x + place.x, headPlace.y + place.y, headPlace.z + place.z },
				{ headTurn.x, headTurn.y, headTurn.z, headTurn.w } };
			sample.handTracked[hand] = true;
		};
		handAt({ 0.18f, -0.28f, -0.40f }, HandRightAim);
		handAt({ 0.18f, -0.32f, -0.30f }, HandRightGrip);
		handAt({ -0.12f, -0.26f, -0.55f }, HandLeftAim);
		_head.orientation = headTurn;
		_headValid = true;
		{
			std::lock_guard<std::mutex> lock(box.sampleMutex);
			box.sample = sample;
		}

		// The controllers, played by a script (the time in seconds since the session began):
		// A (the inventory) at 3, A again at 6 (on the monitor: closes it), the left stick pushed
		// forward from 8 to 9 (walking), the right trigger at 10.5 (firing), the right stick right
		// at 12 (a turn); the left trigger at 13 (the cursor freed: the C key), the ray on the HUD's
		// INV button from 13.4 to 15 and the right trigger at 14 (a click on it: the inventory),
		// A at 16 (closes it), the left trigger at 17 (mouse look back), then in the first session
		// Y held from 18 for 1.3 s (the turn ended: the aliens' turn plays on the pane), in the later
		// ones B at 18 (back to the pane); the ray on the monitor's middle meanwhile. The headset's script: A at 3 and 6,
		// B at 9, nothing else.
		bool held[ButtonCount] = {};
		XrVector2f move = { 0.0f, 0.0f }, turn = { 0.0f, 0.0f };
		auto between = [seconds](float from, float to) { return seconds >= from && seconds < to; };
		held[ButtonA] = between(3.0f, 3.2f) || between(6.0f, 6.2f) || (!headsetScript && between(16.0f, 16.2f));
		if (headsetScript)
		{
			held[ButtonB] = between(9.0f, 9.2f);
		}
		else
		{
			held[ButtonTriggerRight] = between(10.5f, 10.7f) || between(14.0f, 14.2f);
			held[ButtonTriggerLeft] = between(13.0f, 13.2f) || between(17.0f, 17.2f);
			held[ButtonB] = !endsTheTurn && between(18.0f, 18.2f);
			held[ButtonY] = endsTheTurn && between(18.0f, 19.3f);
			move.y = between(8.0f, 9.0f) ? 1.0f : 0.0f;
			turn.x = between(12.0f, 12.3f) ? 1.0f : 0.0f;
		}
		ControllerState rays;
		rays.rayOnMonitor = true;
		rays.monitorX = rays.monitorY = 0.5f;
		// (INV: x 47 to 68, y 1 to 12 on the 640x400 HUD picture)
		rays.rayOnHud = !headsetScript && between(13.4f, 15.0f);
		rays.hudX = 57.5f / 640.0f;
		rays.hudY = 6.5f / 400.0f;
		publishControllers(held, move, turn, rays, true);
		// The HUD held still in the room while the cursor is free, as the real loop places it (see frame).
		if (box.hudHeld != hudHeldWas)
		{
			hudHeldWas = box.hudHeld;
			log(ANDROID_LOG_INFO, "TEST BUILD: the HUD %s", hudHeldWas ? "held still in the room (the cursor free)" : "fixed where the head looks again");
		}

		// The pictures: taken, checked, counted, and a few saved (to see the colours and that no row is upside down).
		if (box.eyes.take())
		{
			const EyePictures &pictures = box.eyes.front();
			++_statsNewEyes;
			if (!sizesLogged)
			{
				sizesLogged = true;
				log(ANDROID_LOG_INFO, "TEST BUILD: the first eye pictures: %dx%d and %dx%d, %zu and %zu pixels",
					pictures.eyes[0].width, pictures.eyes[0].height, pictures.eyes[1].width, pictures.eyes[1].height,
					pictures.eyes[0].pixels.size(), pictures.eyes[1].pixels.size());
				// Their depth: how many values, the nearest (0 at DepthNearMetres) and how many are sky (1).
				float nearest = 1.0f;
				size_t sky = 0;
				for (float value : pictures.depth[0])
				{
					nearest = std::min(nearest, value);
					sky += value >= 1.0f ? 1 : 0;
				}
				log(ANDROID_LOG_INFO, "TEST BUILD: the left eye's depth: %zu values, the nearest %.4f (%.2f m), sky %zu",
					pictures.depth[0].size(), nearest, nearest < 1.0f ? DepthNearMetres / (1.0f - nearest * (DepthFarMetres - DepthNearMetres) / DepthFarMetres) : 0.0f, sky);
			}
			if (between(2.0f, 2.1f))
			{
				saveTestPicture(pictures.eyes[0], "vrtest_eye_left.bmp");
				saveTestPicture(pictures.eyes[1], "vrtest_eye_right.bmp");
				if (box.smoothing && !smoothedSaved)
				{
					smoothedSaved = true;
					saveSmoothedTestPicture(pictures.eyes[0], "vrtest_eye_left_smoothed.bmp");
				}
			}
		}
		if (box.hud.take())
		{
			++_statsUploads;
			if (between(2.0f, 2.1f))
			{
				saveTestPicture(box.hud.front(), "vrtest_hud.bmp");
			}
			if (between(13.8f, 13.9f))
			{
				saveTestPicture(box.hud.front(), "vrtest_hud_cursor.bmp");
			}
		}
		if (box.monitor.take())
		{
			++_statsUploads;
			if (between(2.0f, 2.1f) || between(4.5f, 4.6f))
			{
				saveTestPicture(box.monitor.front(), seconds < 3.0f ? "vrtest_monitor_3d.bmp" : "vrtest_monitor_inventory.bmp");
			}
		}
		++_statsFrames;
		_lastFrameEnd = Clock::now();
		reportStatistics();
	}
	box.running = false;
	log(ANDROID_LOG_INFO, "TEST BUILD: the acted-out session ends");
}
#endif

/**
 * Logs a failed OpenXR call, with its name and the result's name.
 * @param result What it returned.
 * @param what The call.
 * @return True when it succeeded.
 */
bool Session::check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
	{
		return true;
	}
	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (_instance == XR_NULL_HANDLE || xrResultToString(_instance, result, name) != XR_SUCCESS)
	{
		std::snprintf(name, sizeof(name), "%d", (int)result);
	}
	log(ANDROID_LOG_ERROR, "%s failed: %s", what, name);
	return false;
}

/**
 * Makes a swapchain for a picture of this size, or remakes it when the size changed (the
 * setting "VR eye picture", a new HUD); the images are textures of our context.
 * @param chain The swapchain.
 * @param width The picture's width.
 * @param height Its height.
 * @return True when there is one of this size.
 */
bool Session::makeSwapchain(Swapchain &chain, int width, int height, bool depth)
{
	if (width <= 0 || height <= 0)
	{
		return false;
	}
	if (chain.handle != XR_NULL_HANDLE && chain.width == width && chain.height == height)
	{
		return true;
	}
	if (chain.handle != XR_NULL_HANDLE)
	{
		// (Uploads into it must have finished before it goes.)
		glFinish();
		xrDestroySwapchain(chain.handle);
		chain = Swapchain();
	}
	XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
	createInfo.format = depth ? _depthFormat : _format;
	createInfo.sampleCount = 1;
	createInfo.width = (uint32_t)width;
	createInfo.height = (uint32_t)height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	if (!check(xrCreateSwapchain(_session, &createInfo, &chain.handle), "xrCreateSwapchain"))
	{
		chain = Swapchain();
		return false;
	}
	uint32_t count = 0;
	bool listed = check(xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr), "xrEnumerateSwapchainImages") && count > 0;
	if (listed)
	{
		chain.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
		listed = check(xrEnumerateSwapchainImages(chain.handle, count, &count, (XrSwapchainImageBaseHeader *)chain.images.data()), "xrEnumerateSwapchainImages") && count > 0;
	}
	if (!listed)
	{
		xrDestroySwapchain(chain.handle);
		chain = Swapchain();
		return false;
	}
	chain.width = width;
	chain.height = height;
	log(ANDROID_LOG_INFO, "swapchain %dx%d%s, %u images", width, height, depth ? " (depth)" : "", count);
	return true;
}

/**
 * Makes the smoother (FirstPersonXrSmoothing.h) on our context the first time the eyes are to be smoothed, and the GPU
 * timers round it when the chip has them (GL_EXT_disjoint_timer_query); logs which filter and how big.
 * @return False when there is none (it couldn't be made, or failed once): the eyes go as they are.
 */
bool Session::smootherReady()
{
	if (_smootherFailed)
	{
		return false;
	}
	if (_smoother.ready())
	{
		return true;
	}
	std::string error;
	if (!_smoother.create(EyeSmoothingFilter, error))
	{
		_smootherFailed = true;
		log(ANDROID_LOG_ERROR, "VR smoothing: the filter couldn't be made (%s); the eyes go as they are", error.c_str());
		return false;
	}
	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query"))
	{
		_beginQuery = (void (GL_APIENTRY *)(GLenum, GLuint))eglGetProcAddress("glBeginQueryEXT");
		_endQuery = (void (GL_APIENTRY *)(GLenum))eglGetProcAddress("glEndQueryEXT");
		_hasTimerQuery = _beginQuery && _endQuery;
		if (_hasTimerQuery)
		{
			glGenQueries(4, _smoothQueries);
		}
	}
	log(ANDROID_LOG_INFO, "VR smoothing: %s, each eye drawn %d times bigger by the graphics chip (its GPU time %s)",
		EyeSmoothingFilter == SmoothingFilter::Sabr ? "SABR" : "FSR 1", SmoothingScale, _hasTimerQuery ? "in the statistics" : "not measurable here");
	return true;
}

/**
 * Draws a new eye pair smoothed into the eye swapchains, each picture smoothingScaleFor times bigger (the field of view
 * the same: the projection layer shows the whole image), and its depth with it into depth swapchains of that size, each
 * value over its block. A GPU timer round it all, when there is one free.
 * @param pictures The pair.
 * @param withDepth Its depth to go too.
 * @param depthDone Set to whether the depth went.
 * @return False when the pair couldn't go in (a failed smoothing: from then on the eyes go as they are).
 */
bool Session::smoothPair(const EyePictures &pictures, bool withDepth, bool &depthDone)
{
	depthDone = withDepth;
	GLuint query = 0;
	if (_hasTimerQuery && !_smoothQueryBusy[_smoothQueryNext])
	{
		query = _smoothQueries[_smoothQueryNext];
		_beginQuery(TimeElapsedQuery, query);
	}
	bool both = true;
	for (int eye = 0; eye < 2 && both; ++eye)
	{
		const LayerPicture &picture = pictures.eyes[eye];
		const int scale = smoothingScaleFor(picture.width, picture.height);
		const int width = picture.width * scale, height = picture.height * scale;
		uint32_t index = 0;
		if (!makeSwapchain(_eyeChains[eye], width, height) || (int)picture.pixels.size() < picture.width * picture.height || !acquireImage(_eyeChains[eye], index))
		{
			both = false;
			break;
		}
		const bool drawn = _smoother.drawColour(picture.pixels.data(), picture.width, picture.height, _eyeChains[eye].images[index].image, width, height, _format == FormatSrgb);
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		const bool released = check(xrReleaseSwapchainImage(_eyeChains[eye].handle, &releaseInfo), "xrReleaseSwapchainImage");
		if (!drawn)
		{
			_smootherFailed = true;
			log(ANDROID_LOG_ERROR, "VR smoothing failed (OpenGL); the eyes go as they are from now on");
		}
		both = drawn && released;
		_eyeChains[eye].hasImage = _eyeChains[eye].hasImage || released;
		// Its depth, at the same size.
		if (both && depthDone)
		{
			uint32_t depthIndex = 0;
			if ((int)pictures.depth[eye].size() < picture.width * picture.height || !makeSwapchain(_depthChains[eye], width, height, true)
				|| !acquireImage(_depthChains[eye], depthIndex))
			{
				depthDone = false;
				continue;
			}
			const bool depthDrawn = _smoother.drawDepth(pictures.depth[eye].data(), picture.width, picture.height, _depthChains[eye].images[depthIndex].image, width, height);
			const bool depthReleased = check(xrReleaseSwapchainImage(_depthChains[eye].handle, &releaseInfo), "xrReleaseSwapchainImage");
			_depthChains[eye].hasImage = _depthChains[eye].hasImage || depthReleased;
			if (!depthDrawn && _depthFormat)
			{
				// (A chip that can't draw into the runtime's depth images: the eyes go on without depth.)
				log(ANDROID_LOG_WARN, "VR smoothing: the eyes' depth couldn't be drawn; the eyes go on without depth");
				_depthFormat = 0;
			}
			depthDone = depthDrawn && depthReleased;
		}
	}
	if (query)
	{
		_endQuery(TimeElapsedQuery);
		_smoothQueryBusy[_smoothQueryNext] = true;
		_smoothQueryNext = (_smoothQueryNext + 1) % 4;
	}
	if (both)
	{
		++_statsSmoothed;
	}
	depthDone = depthDone && both;
	return both;
}

/**
 * Reads the GPU timers round the smoothing that have finished (a timer the chip says was disturbed is dropped), for the
 * statistics.
 */
void Session::readSmoothingTimers()
{
	if (!_hasTimerQuery)
	{
		return;
	}
	GLint disjoint = 0;
	glGetIntegerv(GpuDisjoint, &disjoint);
	for (int i = 0; i < 4; ++i)
	{
		if (!_smoothQueryBusy[i])
		{
			continue;
		}
		GLuint available = 0;
		glGetQueryObjectuiv(_smoothQueries[i], GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
		{
			continue;
		}
		GLuint nanoseconds = 0;
		glGetQueryObjectuiv(_smoothQueries[i], GL_QUERY_RESULT, &nanoseconds);
		_smoothQueryBusy[i] = false;
		if (!disjoint)
		{
			const double ms = nanoseconds / 1.0e6;
			_statsSmoothGpuMs += ms;
			_statsSmoothGpuWorstMs = std::max(_statsSmoothGpuWorstMs, ms);
			++_statsSmoothTimed;
		}
	}
}

/**
 * Takes a swapchain's next image and waits until it may be written. An image not waited for may
 * not be released, so when that fails the swapchain goes (a new one comes with the next picture).
 * @param chain The swapchain.
 * @param index Set to the image's place in the swapchain.
 * @return True when the image may be written (then it must be released).
 */
bool Session::acquireImage(Swapchain &chain, uint32_t &index)
{
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!check(xrAcquireSwapchainImage(chain.handle, &acquireInfo, &index), "xrAcquireSwapchainImage"))
	{
		return false;
	}
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!check(xrWaitSwapchainImage(chain.handle, &waitInfo), "xrWaitSwapchainImage") || index >= chain.images.size())
	{
		// (An image not waited for may not be released: the swapchain goes, a new one comes with the next picture.)
		glFinish();
		xrDestroySwapchain(chain.handle);
		chain = Swapchain();
		return false;
	}
	return true;
}

/**
 * Uploads an eye's depth (0 to 1, see DepthNearMetres; the bottom row first) into a depth
 * swapchain's next image, in its format's type (32-bit floats, or 24 or 16 bits).
 * @param chain The depth swapchain, the eye's size.
 * @param depth The depth.
 * @return True when it went in.
 */
bool Session::uploadDepth(Swapchain &chain, const std::vector<float> &depth)
{
	if ((int)depth.size() < chain.width * chain.height)
	{
		return false;
	}
	uint32_t index = 0;
	if (!acquireImage(chain, index))
	{
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, chain.images[index].image);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (_depthFormat == FormatDepth32F)
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chain.width, chain.height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
	}
	else if (_depthFormat == FormatDepth24)
	{
		std::vector<GLuint> values(depth.size());
		for (size_t i = 0; i < depth.size(); ++i)
		{
			values[i] = (GLuint)(depth[i] * 4294967295.0);
		}
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chain.width, chain.height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, values.data());
	}
	else
	{
		std::vector<GLushort> values(depth.size());
		for (size_t i = 0; i < depth.size(); ++i)
		{
			values[i] = (GLushort)(depth[i] * 65535.0f + 0.5f);
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chain.width, chain.height, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, values.data());
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	const GLenum error = glGetError();
	if (error != GL_NO_ERROR)
	{
		// (A driver that takes no uploaded depth: the eyes go on without it.)
		log(ANDROID_LOG_WARN, "uploading the eyes' depth: OpenGL error 0x%x; the eyes go on without depth", error);
		_depthFormat = 0;
	}
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if (!check(xrReleaseSwapchainImage(chain.handle, &releaseInfo), "xrReleaseSwapchainImage"))
	{
		return false;
	}
	chain.hasImage = true;
	return error == GL_NO_ERROR;
}

/**
 * Uploads a picture into a swapchain's next image (acquire, wait, glTexSubImage2D, release); a
 * layer may show the swapchain from then on.
 * @param chain The swapchain, the picture's size.
 * @param picture The picture: RGBA, the bottom row first.
 * @return True when it went in.
 */
bool Session::upload(Swapchain &chain, const LayerPicture &picture)
{
	if ((int)picture.pixels.size() < chain.width * chain.height)
	{
		return false;
	}
	uint32_t index = 0;
	if (!acquireImage(chain, index))
	{
		return false;
	}
	{
		glBindTexture(GL_TEXTURE_2D, chain.images[index].image);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chain.width, chain.height, GL_RGBA, GL_UNSIGNED_BYTE, picture.pixels.data());
		glBindTexture(GL_TEXTURE_2D, 0);
		const GLenum error = glGetError();
		if (error != GL_NO_ERROR && !_glErrorLogged)
		{
			_glErrorLogged = true;
			log(ANDROID_LOG_ERROR, "uploading a picture: OpenGL error 0x%x (logged once)", error);
		}
	}
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if (!check(xrReleaseSwapchainImage(chain.handle, &releaseInfo), "xrReleaseSwapchainImage"))
	{
		return false;
	}
	chain.hasImage = true;
	return true;
}

/**
 * Ends everything this thread made, the last first: the swapchains, the action set (its actions
 * and spaces go with it), the spaces, the session, the instance, and our context (the EGL
 * display stays initialised: SDL uses it too).
 */
void Session::destroy()
{
	if (_context != EGL_NO_CONTEXT)
	{
		glFinish();
		_smoother.destroy();
		if (_hasTimerQuery)
		{
			glDeleteQueries(4, _smoothQueries);
			_hasTimerQuery = false;
		}
	}
	for (Swapchain *chain : { &_eyeChains[0], &_eyeChains[1], &_depthChains[0], &_depthChains[1], &_hudChain, &_monitorChain })
	{
		if (chain->handle != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(chain->handle);
			*chain = Swapchain();
		}
	}
	if (_aimSpace != XR_NULL_HANDLE) xrDestroySpace(_aimSpace);
	if (_rightGripSpace != XR_NULL_HANDLE) xrDestroySpace(_rightGripSpace);
	if (_leftAimSpace != XR_NULL_HANDLE) xrDestroySpace(_leftAimSpace);
	if (_actionSet != XR_NULL_HANDLE) xrDestroyActionSet(_actionSet);
	if (_viewSpace != XR_NULL_HANDLE) xrDestroySpace(_viewSpace);
	if (_localSpace != XR_NULL_HANDLE) xrDestroySpace(_localSpace);
	if (_session != XR_NULL_HANDLE) xrDestroySession(_session);
	if (_instance != XR_NULL_HANDLE) xrDestroyInstance(_instance);
	_aimSpace = _rightGripSpace = _leftAimSpace = _viewSpace = _localSpace = XR_NULL_HANDLE;
	_actionSet = XR_NULL_HANDLE;
	_session = XR_NULL_HANDLE;
	_instance = XR_NULL_HANDLE;
	if (_display != EGL_NO_DISPLAY)
	{
		eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (_surface != EGL_NO_SURFACE) eglDestroySurface(_display, _surface);
		if (_context != EGL_NO_CONTEXT) eglDestroyContext(_display, _context);
		eglReleaseThread();
	}
	_surface = EGL_NO_SURFACE;
	_context = EGL_NO_CONTEXT;
}

/// The block both threads share.
Shared sharedBlock;
/// The OpenXR thread, and whether it still runs (under threadMutex; threadDone tells when it ends).
std::thread sessionThread;
std::mutex threadMutex;
std::condition_variable threadDone;
bool threadRunning = false;
/// The activity the thread's session was made with (a global reference).
jobject sessionActivity = nullptr;
/// The global references of the earlier sessions' activities, kept for the game's life: each instance was made with
/// its activity, and the runtime may hold that reference after the instance is gone (a destroyed activity is a small
/// object; one is kept a trip into VR).
std::vector<jobject> earlierActivities;
/// The application's context, for Khronos's loader (a global reference kept for the game's life: see initialiseLoader).
jobject applicationContext = nullptr;

/**
 * The application's context of an activity (Context.getApplicationContext), as a global reference.
 * @param env The Java environment.
 * @param activity The activity.
 * @return The reference, or null.
 */
jobject applicationContextOf(JNIEnv *env, jobject activity)
{
	jobject context = nullptr;
	jclass type = env->GetObjectClass(activity);
	jmethodID method = type ? env->GetMethodID(type, "getApplicationContext", "()Landroid/content/Context;") : nullptr;
	jobject local = method ? env->CallObjectMethod(activity, method) : nullptr;
	if (env->ExceptionCheck())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		local = nullptr;
	}
	if (local)
	{
		context = env->NewGlobalRef(local);
		env->DeleteLocalRef(local);
	}
	if (type)
	{
		env->DeleteLocalRef(type);
	}
	return context;
}

/// Keeps the session's activity reference among the earlier ones (see earlierActivities), the session having none.
void keepSessionActivity()
{
	if (sessionActivity)
	{
		earlierActivities.push_back(sessionActivity);
		sessionActivity = nullptr;
	}
}

}

/**
 * The one shared block.
 */
Shared &shared()
{
	return sharedBlock;
}

/**
 * Writes a line to the system log (logcat tag OpenXcom3D), starting "VR ", and to the game's (openxcom.log, through
 * logToGame: the OpenXR thread's lines too survive in the owner's log).
 * @param priority Android's log priority (ANDROID_LOG_INFO, ...).
 * @param format A printf format, and its values.
 */
void log(int priority, const char *format, ...)
{
	char text[1024];
	va_list values;
	va_start(values, format);
	std::vsnprintf(text, sizeof(text), format, values);
	va_end(values);
	__android_log_print(priority, "OpenXcom3D", "VR %s", text);
	logToGame(std::string("OpenXcom3D: VR ") + text);
}

/**
 * Starts the OpenXR thread for the immersive activity: the shared block made fresh (a new
 * session sees nothing of the last), then the thread with a session made with this activity (and
 * the application's context for Khronos's loader, found the first time).
 * @param env The UI thread's Java environment.
 * @param activity The immersive activity.
 */
void startSession(JNIEnv *env, jobject activity)
{
	std::unique_lock<std::mutex> lock(threadMutex);
	if (threadRunning)
	{
		// (The last one is still ending: give it the few seconds stopSession would.)
		log(ANDROID_LOG_WARN, "the last OpenXR thread still runs: waiting for it");
		shared().stopRequested = true;
		threadDone.wait_for(lock, std::chrono::seconds(4), [] { return !threadRunning; });
		if (threadRunning)
		{
			log(ANDROID_LOG_ERROR, "the last OpenXR thread didn't end: no VR this time");
			shared().failed = true;
			shared().finished = true;
			return;
		}
	}
	if (sessionThread.joinable())
	{
		sessionThread.join();
	}
	keepSessionActivity();
	JavaVM *vm = nullptr;
	env->GetJavaVM(&vm);
	sessionActivity = env->NewGlobalRef(activity);
	if (!applicationContext)
	{
		applicationContext = applicationContextOf(env, activity);
	}

	// A fresh shared block.
	Shared &box = shared();
	box.eyes.clear();
	box.hud.clear();
	box.monitor.clear();
	box.stopRequested = false;
	box.placeQuads = false;
	box.running = false;
	box.failed = false;
	box.finished = false;
	{
		std::lock_guard<std::mutex> sampleLock(box.sampleMutex);
		box.sample = HeadSample();
	}
	{
		std::lock_guard<std::mutex> controllerLock(box.controllerMutex);
		box.controllers = ControllerState();
	}

	threadRunning = true;
	jobject activityForThread = sessionActivity, contextForThread = applicationContext ? applicationContext : sessionActivity;
	sessionThread = std::thread([vm, activityForThread, contextForThread]
	{
		Session session(vm, activityForThread, contextForThread);
		session.run();
		std::lock_guard<std::mutex> doneLock(threadMutex);
		threadRunning = false;
		shared().finished = true;
		threadDone.notify_all();
	});
	log(ANDROID_LOG_INFO, "the OpenXR thread starts");
}

/**
 * Ends the OpenXR thread: asks its session to end and waits for the thread, four seconds at
 * most (the activity's reference is kept even after: see earlierActivities). Only when the
 * session is this activity's: an older immersive activity whose end comes after a newer one
 * started (F8 pressed again at once) leaves the newer one's session alone.
 * @param env The UI thread's Java environment.
 * @param activity The immersive activity that goes.
 */
void stopSession(JNIEnv *env, jobject activity)
{
	std::unique_lock<std::mutex> lock(threadMutex);
	if (sessionActivity && activity && !env->IsSameObject(sessionActivity, activity))
	{
		log(ANDROID_LOG_INFO, "an older immersive activity went: the session is a newer one's, it goes on");
		return;
	}
	shared().stopRequested = true;
	if (!threadDone.wait_for(lock, std::chrono::seconds(4), [] { return !threadRunning; }))
	{
		// (It may still use the activity: its reference stays, and the thread is left to end by itself.)
		log(ANDROID_LOG_ERROR, "the OpenXR thread didn't end in 4 s: left to end by itself");
		if (sessionThread.joinable())
		{
			sessionThread.detach();
		}
		keepSessionActivity();
		return;
	}
	lock.unlock();
	if (sessionThread.joinable())
	{
		sessionThread.join();
	}
	lock.lock();
	keepSessionActivity();
}

/**
 * Is the OpenXR thread still running?
 */
bool sessionThreadAlive()
{
	std::lock_guard<std::mutex> lock(threadMutex);
	return threadRunning;
}

}

}

}

// ---- The immersive activity's native calls (org.openxcom.extended.VrActivity) ----

/// The activity was created: its OpenXR session starts on a thread of its own.
extern "C" JNIEXPORT void JNICALL Java_org_openxcom_extended_VrActivity_nativeOnCreate(JNIEnv *env, jclass, jobject activity)
{
	OpenXcom::FirstPerson::Xr::startSession(env, activity);
}

/// The activity goes: its session ends and the thread with it.
extern "C" JNIEXPORT void JNICALL Java_org_openxcom_extended_VrActivity_nativeOnDestroy(JNIEnv *env, jclass, jobject activity)
{
	OpenXcom::FirstPerson::Xr::stopSession(env, activity);
}

/// The activity's other moments (resumed, paused, focus), for the log.
extern "C" JNIEXPORT void JNICALL Java_org_openxcom_extended_VrActivity_nativeOnLifecycle(JNIEnv *env, jclass, jstring what)
{
	const char *text = env->GetStringUTFChars(what, nullptr);
	OpenXcom::FirstPerson::Xr::log(ANDROID_LOG_INFO, "activity: %s", text ? text : "?");
	if (text)
	{
		env->ReleaseStringUTFChars(what, text);
	}
}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
