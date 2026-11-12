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
 * FirstPersonGpu.cpp: the 3D view's rays on the graphics card (see FirstPersonGpu.h).
 *
 * What goes up to the card, all as plain buffers the shader indexes by number:
 *  - the voxel models, each once, the first time a tile shows it: its colours (a byte a voxel),
 *    its bit masks (glass, look-only glass, holes in the art, a UFO part's lights) and a line
 *    of facts about it (a ghost? its own ghost model? from what height it glows?);
 *  - the solid masks, each once (a bit a voxel: any of a tile's parts solid there);
 *  - every frame, the tile snapshot: the byte per tile the walk reads first, and two texels a
 *    tile with its parts' model numbers, the box round its solid voxels, its solid mask and
 *    what else the walk reads.
 *  - once each as well: a steady UFO model's open faces, the wall paints, the crafts' mirrors;
 *  - every frame they changed: the scene's colour tables, and the camera and the scene's
 *    numbers as uniforms.
 * The shaders (FirstPersonGpuShaders.h) are the CPU renderer's functions line for line.
 *
 * How to change it:
 *  - A change to the CPU's walk or shading (FirstPersonViewRayWalk.h, shadeHit and what it
 *    calls): make the same change in the shader function of the same name. A new constant: add
 *    it to shaderConstants below. Then prove it: OPENXCOM3D_GPU_SHADOW=1 runs both renderers
 *    side by side and the log counts the rays and colours that differ (see FirstPersonViewGpu.cpp).
 *  - New data for the shader: a field in Frame (FirstPersonGpu.h), filled in
 *    FirstPersonViewGpuAccess::fillFrame; then here a uniform (setScene), a free number of a
 *    tile's sixteen (encodeTiles), a row of the colour tables (encodeTables), or a buffer of
 *    its own (the enum in State, start, build's sampler names).
 *  - A look the CPU renderer doesn't have (the card can afford more): a setting, a uniform for
 *    it, and a branch in the shader that leaves the CPU's path untouched when it is off, so
 *    the check against the CPU still holds for the default.
 *  - Another system's context: GpuContext::create and destroy (EGL on Linux, CGL on macOS).
 *
 * What can go wrong, and what happens then: no card, a software OpenGL, a missing function, a
 * shader the driver won't build, an OpenGL error while drawing: the log says which, castRays
 * returns false, and the view casts its rays on the CPU (for good, after a failure). A frame
 * beyond the shader's fixed limits (see draw) is the CPU's too, that frame only.
 */
#include "FirstPersonGpu.h"
#include "FirstPersonGpuGl.h"
#include "FirstPersonGpuShaders.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#ifndef _WIN32
#include <pthread.h>
#endif
#include "../Engine/Logger.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

// ---- OpenGL, looked up at run time ----

namespace Gl
{
#define GL_DEFINE(result, name, arguments) result (GL_CALL *name) arguments = nullptr;
GL_FUNCTIONS(GL_DEFINE)
#undef GL_DEFINE
}

/**
 * Looks every OpenGL function up in the context's driver.
 * @return False if one is missing (the log says which).
 */
bool GpuContext::loadFunctions()
{
	bool all = true;
#define GL_LOAD(result, name, arguments) \
	Gl::name = (decltype(Gl::name))_getProcAddress("gl" #name); \
	if (!Gl::name) { Log(LOG_WARNING) << "OpenXcom3D GPU: the driver has no gl" #name; all = false; }
	GL_FUNCTIONS(GL_LOAD)
#undef GL_LOAD
	return all;
}

#if defined(__linux__) && !defined(__ANDROID__)

/**
 * Opens a hidden OpenGL 3.3 context through EGL, with no window and no X server: on the
 * machine's graphics devices as EGL lists them (EGL_EXT_device_enumeration), the one asked for
 * by number; where EGL can't list them, on its default display.
 */
bool GpuContext::create(int device)
{
	// EGL's own types and constants (its values), kept local: nothing else needs them.
	typedef void *(*GetDisplay)(void *);
	typedef unsigned (*Initialize)(void *, int *, int *);
	typedef unsigned (*BindApi)(unsigned);
	typedef unsigned (*ChooseConfig)(void *, const int *, void **, int, int *);
	typedef void *(*CreateContext)(void *, void *, void *, const int *);
	typedef void *(*CreatePbuffer)(void *, void *, const int *);
	typedef unsigned (*MakeCurrent)(void *, void *, void *, void *);
	typedef const char *(*QueryString)(void *, int);
	typedef int (*GetError)();
	typedef unsigned (*QueryDevices)(int, void **, int *);
	typedef void *(*GetPlatformDisplay)(unsigned, void *, const int *);
	typedef const char *(*QueryDeviceString)(void *, int);
	const int EglExtensions = 0x3055, EglNone = 0x3038, EglSurfaceType = 0x3033, EglPbufferBit = 1, EglRenderableType = 0x3040, EglOpenGlBit = 8;
	const int EglContextMajor = 0x3098, EglContextMinor = 0x30FB, EglProfileMask = 0x30FD, EglCoreProfile = 1, EglWidth = 0x3057, EglHeight = 0x3056;
	const unsigned EglOpenGlApi = 0x30A2, EglPlatformDevice = 0x313F;
	const int EglDrmDeviceFile = 0x3233;

	_library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!_library)
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: no libEGL.so.1 on this machine";
		return false;
	}
	auto symbol = [this](const char *name) { return dlsym(_library, name); };
	const auto getDisplay = (GetDisplay)symbol("eglGetDisplay");
	const auto initialize = (Initialize)symbol("eglInitialize");
	const auto bindApi = (BindApi)symbol("eglBindAPI");
	const auto chooseConfig = (ChooseConfig)symbol("eglChooseConfig");
	const auto createContext = (CreateContext)symbol("eglCreateContext");
	const auto createPbuffer = (CreatePbuffer)symbol("eglCreatePbufferSurface");
	const auto makeCurrent = (MakeCurrent)symbol("eglMakeCurrent");
	const auto queryString = (QueryString)symbol("eglQueryString");
	const auto getError = (GetError)symbol("eglGetError");
	_getProcAddress = (void *(*)(const char *))symbol("eglGetProcAddress");
	if (!getDisplay || !initialize || !bindApi || !chooseConfig || !createContext || !createPbuffer || !makeCurrent || !queryString || !getError || !_getProcAddress)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: libEGL.so.1 lacks a function";
		return false;
	}

	// The machine's graphics devices, where EGL lists them: the one asked for.
	const char *clientExtensions = queryString(nullptr, EglExtensions);
	const std::string extensions = clientExtensions ? clientExtensions : "";
	if (extensions.find("EGL_EXT_device_enumeration") != std::string::npos && extensions.find("EGL_EXT_platform_device") != std::string::npos)
	{
		const auto queryDevices = (QueryDevices)_getProcAddress("eglQueryDevicesEXT");
		const auto getPlatformDisplay = (GetPlatformDisplay)_getProcAddress("eglGetPlatformDisplayEXT");
		const auto queryDeviceString = (QueryDeviceString)_getProcAddress("eglQueryDeviceStringEXT");
		void *devices[16];
		int count = 0;
		if (queryDevices && getPlatformDisplay && queryDevices(16, devices, &count) && count > 0)
		{
			for (int index = 0; index < count; ++index)
			{
				const char *file = queryDeviceString ? queryDeviceString(devices[index], EglDrmDeviceFile) : nullptr;
				Log(LOG_INFO) << "OpenXcom3D GPU: device " << index << (file ? std::string(" ") + file : std::string(" (no file)"));
			}
			if (device >= count)
			{
				return false; // no such device
			}
			_display = getPlatformDisplay(EglPlatformDevice, devices[std::max(device, 0)], nullptr);
		}
	}
	if (!_display && device > 0)
	{
		return false; // EGL can't list devices: only its default one
	}
	if (!_display)
	{
		_display = getDisplay(nullptr);
	}
	int major = 0, minor = 0;
	if (!_display || !initialize(_display, &major, &minor) || !bindApi(EglOpenGlApi))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: EGL would not start (error " << std::hex << getError() << ")";
		return false;
	}

	// A context with no window: on no surface at all where the driver allows, else on a 1x1 off-screen one.
	const int configWanted[] = { EglSurfaceType, EglPbufferBit, EglRenderableType, EglOpenGlBit, EglNone };
	void *config = nullptr;
	int configCount = 0;
	if (!chooseConfig(_display, configWanted, &config, 1, &configCount) || configCount < 1)
	{
		config = nullptr; // no config: fine with EGL_KHR_no_config_context, and the surfaceless way below
	}
	const int contextWanted[] = { EglContextMajor, 3, EglContextMinor, 3, EglProfileMask, EglCoreProfile, EglNone };
	_context = createContext(_display, config, nullptr, contextWanted);
	if (!_context)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no OpenGL 3.3 context (EGL error " << std::hex << getError() << ")";
		return false;
	}
	if (!makeCurrent(_display, nullptr, nullptr, _context))
	{
		const int surfaceWanted[] = { EglWidth, 1, EglHeight, 1, EglNone };
		_surface = config ? createPbuffer(_display, config, surfaceWanted) : nullptr;
		if (!_surface || !makeCurrent(_display, _surface, _surface, _context))
		{
			Log(LOG_WARNING) << "OpenXcom3D GPU: the context can't be used without a window (EGL error " << std::hex << getError() << ")";
			return false;
		}
	}
	if (!loadFunctions())
	{
		return false;
	}
	_name = std::string((const char *)Gl::GetString(Gl::RENDERER)) + " (OpenGL " + (const char *)Gl::GetString(Gl::VERSION) + ")";
	return true;
}

bool GpuContext::software() const
{
	for (const char *name : { "llvmpipe", "softpipe", "SwiftShader", "swrast", "Software" })
	{
		if (_name.find(name) != std::string::npos) return true;
	}
	return false;
}

/**
 * Closes the context and EGL's connection to the device.
 */
void GpuContext::destroy()
{
	if (!_library)
	{
		return;
	}
	typedef unsigned (*MakeCurrent)(void *, void *, void *, void *);
	typedef unsigned (*Destroy)(void *, void *);
	typedef unsigned (*Terminate)(void *);
	const auto makeCurrent = (MakeCurrent)dlsym(_library, "eglMakeCurrent");
	const auto destroyContext = (Destroy)dlsym(_library, "eglDestroyContext");
	const auto destroySurface = (Destroy)dlsym(_library, "eglDestroySurface");
	const auto terminate = (Terminate)dlsym(_library, "eglTerminate");
	if (_display)
	{
		if (makeCurrent) makeCurrent(_display, nullptr, nullptr, nullptr);
		if (_surface && destroySurface) destroySurface(_display, _surface);
		if (_context && destroyContext) destroyContext(_display, _context);
		if (terminate) terminate(_display);
	}
	_display = _context = _surface = nullptr;
	// The library stays loaded: drivers leave threads and handlers behind that outlive a dlclose.
	_library = nullptr;
}

#elif defined(__APPLE__)

namespace
{
/// macOS's OpenGL framework, once opened (its functions are looked up in it by name).
void *appleOpenGl = nullptr;
void *appleFunction(const char *name)
{
	return dlsym(appleOpenGl, name);
}
}

/**
 * Opens a hidden OpenGL context on macOS through CGL (the OpenGL framework, opened at run
 * time): a core profile (4.1 on every Mac of the last ten years; ours asks for 3.3's features
 * only), hardware-accelerated, with no window and no view: the rays are drawn into our own
 * textures. macOS has one renderer to offer us, the system's: device 0.
 */
bool GpuContext::create(int device)
{
	if (device > 0)
	{
		return false; // no such device
	}
	// CGL's own types and constants (its values), kept local.
	typedef int (*ChoosePixelFormat)(const int *, void **, int *);
	typedef int (*DestroyPixelFormat)(void *);
	typedef int (*CreateContext)(void *, void *, void **);
	typedef int (*SetCurrentContext)(void *);
	const int PfaAccelerated = 73, PfaAllowOfflineRenderers = 96, PfaOpenGlProfile = 99, PfaColorSize = 8;
	const int ProfileGl4Core = 0x4100, ProfileGl3Core = 0x3200;

	_library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
	if (!_library)
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: no OpenGL framework on this Mac";
		return false;
	}
	appleOpenGl = _library;
	_getProcAddress = appleFunction;
	const auto choosePixelFormat = (ChoosePixelFormat)dlsym(_library, "CGLChoosePixelFormat");
	const auto destroyPixelFormat = (DestroyPixelFormat)dlsym(_library, "CGLDestroyPixelFormat");
	const auto createContext = (CreateContext)dlsym(_library, "CGLCreateContext");
	const auto setCurrentContext = (SetCurrentContext)dlsym(_library, "CGLSetCurrentContext");
	if (!choosePixelFormat || !destroyPixelFormat || !createContext || !setCurrentContext)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: the OpenGL framework lacks a CGL function";
		return false;
	}
	// The newest core profile the Mac has (4.1, else 3.2), on its graphics chip.
	void *pixelFormat = nullptr;
	int formatCount = 0, error = 0;
	for (const int profile : { ProfileGl4Core, ProfileGl3Core })
	{
		const int wanted[] = { PfaOpenGlProfile, profile, PfaAccelerated, PfaAllowOfflineRenderers, PfaColorSize, 24, 0 };
		error = choosePixelFormat(wanted, &pixelFormat, &formatCount);
		if (!error && pixelFormat)
		{
			break;
		}
		pixelFormat = nullptr;
	}
	if (!pixelFormat)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no OpenGL core profile on this Mac (CGL error " << error << ")";
		return false;
	}
	error = createContext(pixelFormat, nullptr, &_context);
	destroyPixelFormat(pixelFormat);
	if (error || !_context || setCurrentContext(_context))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no OpenGL context on this Mac (CGL error " << error << ")";
		return false;
	}
	if (!loadFunctions())
	{
		return false;
	}
	_name = std::string((const char *)Gl::GetString(Gl::RENDERER)) + " (OpenGL " + (const char *)Gl::GetString(Gl::VERSION) + ")";
	return true;
}

/**
 * Closes the context.
 */
void GpuContext::destroy()
{
	if (!_library)
	{
		return;
	}
	typedef int (*SetCurrentContext)(void *);
	typedef int (*DestroyContext)(void *);
	const auto setCurrentContext = (SetCurrentContext)dlsym(_library, "CGLSetCurrentContext");
	const auto destroyContext = (DestroyContext)dlsym(_library, "CGLDestroyContext");
	if (setCurrentContext) setCurrentContext(nullptr);
	if (_context && destroyContext) destroyContext(_context);
	_context = nullptr;
	_library = nullptr; // (the framework stays loaded)
}

bool GpuContext::software() const
{
	return _name.find("Software") != std::string::npos;
}

#elif !defined(_WIN32) // (Windows: FirstPersonGpuWindows.cpp)

/// Not on this system: the CPU renderer draws.
bool GpuContext::create(int)
{
	Log(LOG_INFO) << "OpenXcom3D GPU: not on this system";
	return false;
}

void GpuContext::destroy()
{
}

bool GpuContext::software() const
{
	return false;
}

#endif

// ---- The shaders (FirstPersonGpuShaders.h), and the numbers they share with the C++ ----

namespace
{

const int ModelVoxels = 24 * 16 * 16; // a model's colours, a byte each
const int MaskRows = 24 * 16;         // a bit mask's rows, sixteen bits each
const int ModelMasks = 4;             // a model's masks: glass, look-only glass, holes in the art, lights
const int ModelSideBytes = 1024;      // a model's colours for the faces the art never shows: plan, east, south
const int MaxSwings = 16;             // the doors swinging at once the shader takes
const int TileUints = 16;             // a tile's numbers (four texels of four)
const int TableRows = 14;             // the rows of 256 bytes in the colour tables (see encodeTables)
const int UfoTableRows = 14;          // ... and for each UFO after them: its two tints and twelve soft tints
const int MaxUfos = 8;
const int BufferCount = 13;

/// A number as GLSL reads a float (always with a decimal point or an exponent).
std::string glslFloat(double value)
{
	char text[40];
	snprintf(text, sizeof(text), "%.9g", value);
	std::string result = text;
	if (result.find_first_of(".e") == std::string::npos) result += ".0";
	return result;
}

/**
 * The constants of FirstPersonViewShared.h the shaders use, written out as GLSL: one place for
 * each number, the C++.
 */
std::string shaderConstants()
{
	std::string text;
	auto number = [&](const char *name, double value) { text += std::string("const float ") + name + " = " + glslFloat(value) + ";\n"; };
	auto whole = [&](const char *name, int value) { text += std::string("const int ") + name + " = " + std::to_string(value) + ";\n"; };
	number("ZScale", ZScale);
	number("SeeThroughCropOpacity", SeeThroughCropOpacity);
	number("DistanceShadeVoxels", DistanceShadeVoxels);
	number("DistanceShadeReachParts", DistanceShadeReachParts);
	number("GroundTop", GroundTop);
	number("WaveShades", WaveShades);
	number("FoamFrom", FoamFrom);
	number("DeepFrom", DeepFrom);
	number("OrbitEarthEdge", OrbitEarthEdge);
	number("OrbitAirGlow", OrbitAirGlow);
	number("MoonEarthCos", MoonEarthCos);
	number("MoonEarthSin", MoonEarthSin);
	number("CloudDeckMottle", CloudDeckMottle);
	number("DarkOverflowFrom", DarkOverflowFrom);
	number("DarkOverflowTo", DarkOverflowTo);
	number("UnseenFogDarkening", UnseenFogDarkening);
	number("UnseenFogFromMax", UnseenFogFromMax);
	number("UnseenFogNormal", UnseenFogNormal);
	number("UnseenFogLight", UnseenFogLight);
	number("UnexploredMistFrom", UnexploredMistFrom);
	number("UnexploredMistTo", UnexploredMistTo);
	number("UnexploredMistMax", UnexploredMistMax);
	number("FlareReach", FlareReach);
	number("UfoInsideTint", UfoInsideTint);
	number("UfoInsideTintNight", UfoInsideTintNight);
	whole("HazeSteps", FirstPersonView::SceneColors::HazeSteps);
	whole("WaveSineSteps", WaveSineSteps);
	whole("RockMottleShades", RockMottleShades);
	whole("WaterDarkestShade", WaterDarkestShade);
	whole("DarkCeilingShade", DarkCeilingShade);
	text += "const vec3 MoonEarthDirection = vec3(" + glslFloat(MoonEarthDirection[0]) + ", " + glslFloat(MoonEarthDirection[1]) + ", " + glslFloat(MoonEarthDirection[2]) + ");\n";
	// The wave trains: each one's share of the height and where it flattens out.
	text += "const float WaveTrains[9] = float[9](";
	for (int train = 0; train < 3; ++train)
	{
		text += glslFloat(WaveTrains[train].share) + ", " + glslFloat(WaveTrains[train].fadeFrom) + ", " + glslFloat(WaveTrains[train].fadeTo) + (train < 2 ? ", " : ");\n");
	}
	return text;
}

/// Milliseconds since a moment.
double millisecondsSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}

// ---- The renderer's state, all of it used only on its own thread ----

namespace
{

/**
 * The renderer's thread, with a stack as big as a program's first thread has (16 MB). A
 * graphics driver builds our shader on it, and a thread's usual stack is small (macOS gives
 * every thread but the first half a megabyte): too little there would crash the game, not just
 * fail.
 */
class RenderThread
{
public:
	/// Starts it. False if the system won't.
	template <typename Owner> bool start(Owner *owner, void (Owner::*loop)())
	{
		struct Entry { Owner *owner; void (Owner::*loop)(); };
		Entry *entry = new Entry { owner, loop };
#ifndef _WIN32
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, 16u << 20);
		_started = pthread_create(&_thread, &attributes, [](void *given) -> void * {
			Entry *mine = (Entry *)given;
			(mine->owner->*(mine->loop))();
			delete mine;
			return nullptr;
		}, entry) == 0;
		pthread_attr_destroy(&attributes);
		if (!_started) delete entry;
#else
		_thread = std::thread([entry] { (entry->owner->*(entry->loop))(); delete entry; });
		_started = true;
#endif
		return _started;
	}
	/// Waits for it to end.
	void join()
	{
		if (!_started) return;
		_started = false;
#ifndef _WIN32
		pthread_join(_thread, nullptr);
#else
		_thread.join();
#endif
	}
private:
#ifndef _WIN32
	pthread_t _thread;
#else
	std::thread _thread;
#endif
	bool _started = false;
};

}

struct FirstPersonGpu::State
{
	// The thread that owns the context: it sleeps until handed a job, does it, and says so.
	RenderThread thread;
	std::mutex mutex;
	std::condition_variable wake, finished;
	std::function<void()> job;
	bool hasJob = false, stop = false;

	GpuContext context;
	bool failed = false;    // the card failed: the CPU draws from now on
	bool overLimit = false; // this frame holds more than the shader takes (see draw): the CPU draws it
	bool limitLogged = false;
	// The rays' program, built twice: plain, and with the check's results as a fourth picture.
	struct Program
	{
		Gl::UInt id = 0;
		std::unordered_map<std::string, Gl::Int> locations; // its uniforms, by name
	};
	Program programs[2];
	Gl::UInt resolveProgram = 0; // makes one picture of several rays a pixel (ResolveSource); 0: not built, one ray a pixel
	Program *program = &programs[0];
	Gl::UInt vertexArray = 0;
	// The pictures the rays are drawn into, kept for each size asked (the main view's, the small camera views').
	struct Targets
	{
		Gl::UInt framebuffer = 0;
		Gl::UInt textures[5] = {}; // colour, depth, glow, full colour, results
		bool results = false;
	};
	std::map<std::array<int, 3>, Targets> targets; // by width, height, and which of a frame's pictures (0 the frame's own, 1.. one for each of several rays a pixel)
	std::vector<Uint8> blockColors, blockGlow; // a ray each, before they are spread over their blocks of pixels
	std::vector<float> blockDepths;
	// The buffers and their textures: tile flags, tiles, model colours, model bits, model info, model sides, solid masks, colour tables, the waves' sines.
	enum { TileFlags, Tiles, ModelColors, ModelBits, ModelInfo, ModelSides, Solids, Tables, WaveSines, Exposed, WallPaints, Mirrors, Blend };
	Gl::UInt buffers[BufferCount] = {}, textures[BufferCount] = {};
	Gl::Int maxBufferTexels = 0;

	// What has gone up, kept here too (a buffer that grows is sent again whole).
	std::unordered_map<const FirstPersonView::VoxelModel*, Uint32> modelIds;
	std::vector<Uint8> modelColors, modelSides, modelExposed;
	std::vector<Uint16> modelBits;
	std::vector<Uint32> modelInfo;
	bool modelsChanged = false;
	std::unordered_map<const void*, Uint32> solidIds;
	std::vector<Uint16> solidRows;
	bool solidsChanged = false;
	// The tiles as sent last frame: a tile whose parts and mask are the same keeps their numbers.
	std::vector<std::array<const FirstPersonView::VoxelModel*, 8>> lastParts;
	std::vector<const void*> lastSolid;
	std::vector<Uint32> tileUints, tileUintsSent;
	std::vector<Uint8> tileFlagsSent, tablesSent;
	std::vector<Uint32> wallPaintTexelsSent;
	std::vector<Uint8> tables;
	std::vector<Uint8> blendSent; // the palette the blending table was last made for (see sendBlend)
	std::vector<Uint32> wallPaintTexels;
	std::vector<Sint32> mirrorNumbers;
	std::vector<const void*> mirrorsSent; // the rows of the mirrors as sent last (sent again when they are others)

	void threadLoop();
	void run(const std::function<void()> &work);
	bool start(int device);
	bool build(Program &built, bool results);
	Gl::Int location(const char *name);
	Uint32 modelId(const FirstPersonView::VoxelModel *model);
	Uint32 solidId(FirstPersonView::SolidRows solid);
	void encodeTiles(const Frame &frame);
	void encodeTables(const Frame &frame);
	void sendWallPaints(const Frame &frame);
	void sendBlend(const Frame &frame);
	void sendMirrors(const Frame &frame);
	void sendModels();
	void setScene(const Frame &frame);
	bool setTargets(int width, int height, bool results, int which = 0);
	bool buildResolve();
	bool draw(const Frame &frame, const Output &output, Timing *timing);
};

/**
 * The renderer's thread: jobs one at a time, until told to stop.
 */
void FirstPersonGpu::State::threadLoop()
{
	std::unique_lock<std::mutex> lock(mutex);
	while (true)
	{
		wake.wait(lock, [this] { return hasJob || stop; });
		if (stop)
		{
			break;
		}
		job();
		hasJob = false;
		finished.notify_one();
	}
	context.destroy();
}

/**
 * Does a piece of work on the renderer's thread, and waits for it.
 */
void FirstPersonGpu::State::run(const std::function<void()> &work)
{
	std::unique_lock<std::mutex> lock(mutex);
	job = work;
	hasJob = true;
	wake.notify_one();
	finished.wait(lock, [this] { return !hasJob; });
}

/**
 * Where a uniform of the program is, looked up once.
 */
Gl::Int FirstPersonGpu::State::location(const char *name)
{
	const auto known = program->locations.find(name);
	if (known != program->locations.end())
	{
		return known->second;
	}
	return program->locations[name] = Gl::GetUniformLocation(program->id, name);
}

/**
 * Opens the context and builds everything that lasts: the shader, the buffers and their textures.
 */
bool FirstPersonGpu::State::start(int device)
{
	if (!context.create(device))
	{
		return false;
	}
	// (OPENXCOM3D_GPU_SOFTWARE: a software OpenGL will do, for tests on machines without a card.)
	if (context.software() && !getenv("OPENXCOM3D_GPU_SOFTWARE"))
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: " << context.name() << " draws on the CPU: not used";
		return false;
	}
	const Gl::Enum formats[BufferCount] = { Gl::R8UI, Gl::RGBA32UI, Gl::R8UI, Gl::R16UI, Gl::RGBA32UI, Gl::R8UI, Gl::R16UI, Gl::R8UI, Gl::R32F,
		Gl::R8UI, Gl::RGBA32UI, Gl::R32I, Gl::R8UI };
	Gl::GenBuffers(BufferCount, buffers);
	Gl::GenTextures(BufferCount, textures);
	for (int unit = 0; unit < BufferCount; ++unit)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[unit]);
		Gl::BufferData(Gl::ARRAY_BUFFER, 16, nullptr, Gl::DYNAMIC_DRAW);
		Gl::ActiveTexture(Gl::TEXTURE0 + unit);
		Gl::BindTexture(Gl::TEXTURE_BUFFER, textures[unit]);
		Gl::TexBuffer(Gl::TEXTURE_BUFFER, formats[unit], buffers[unit]);
	}
	if (!buildResolve())
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: no supersampling on this card";
	}
	if (!build(programs[0], false) || !build(programs[1], true))
	{
		return false;
	}
	// The waves' sine table, as FirstPersonViewRayCasting.cpp makes its own.
	std::vector<float> sines(WaveSineSteps);
	for (int step = 0; step < WaveSineSteps; ++step) sines[step] = (float)std::sin(step * 2.0 * Pi / WaveSineSteps);
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[WaveSines]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(sines.size() * 4), sines.data(), Gl::DYNAMIC_DRAW);
	Gl::GenVertexArrays(1, &vertexArray);
	Gl::BindVertexArray(vertexArray);
	Gl::Disable(Gl::DEPTH_TEST);
	Gl::Disable(Gl::BLEND);
	Gl::Disable(Gl::DITHER);
	Gl::PixelStorei(Gl::PACK_ALIGNMENT, 1);
	Gl::PixelStorei(Gl::UNPACK_ALIGNMENT, 1);
	Gl::GetIntegerv(Gl::MAX_TEXTURE_BUFFER_SIZE, &maxBufferTexels);
	// Number 0 means "none" for models and masks: an empty one of each.
	modelColors.assign(ModelVoxels, 0);
	modelSides.assign(ModelSideBytes, 0);
	modelExposed.assign(ModelVoxels, 0);
	modelBits.assign(MaskRows * ModelMasks, 0);
	modelInfo.assign(4, 0);
	solidRows.assign(MaskRows, 0);
	modelsChanged = solidsChanged = true;
	return Gl::GetError() == Gl::NoError;
}

/**
 * Builds the rays' program: the picture-wide triangle, and the walk and the shading, with or
 * without the check's results; its samplers are pointed at the buffers' textures.
 */
bool FirstPersonGpu::State::build(Program &built, bool results)
{
	const std::string raySource = std::string("#version 330 core\n") + (results ? "#define RESULTS\n" : "") + HeaderSource + shaderConstants() + WalkSource + ShadeSource;
	const char *sources[2] = { VertexSource, raySource.c_str() };
	const Gl::Enum kinds[2] = { Gl::VERTEX_SHADER, Gl::FRAGMENT_SHADER };
	built.id = Gl::CreateProgram();
	for (int which = 0; which < 2; ++which)
	{
		const Gl::UInt shader = Gl::CreateShader(kinds[which]);
		Gl::ShaderSource(shader, 1, &sources[which], nullptr);
		Gl::CompileShader(shader);
		Gl::Int compiled = 0;
		Gl::GetShaderiv(shader, Gl::COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			char log[8192] = "";
			Gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
			Log(LOG_ERROR) << "OpenXcom3D GPU: shader " << which << " would not compile: " << log;
			return false;
		}
		Gl::AttachShader(built.id, shader);
		Gl::DeleteShader(shader);
	}
	Gl::LinkProgram(built.id);
	Gl::Int linked = 0;
	Gl::GetProgramiv(built.id, Gl::LINK_STATUS, &linked);
	if (!linked)
	{
		char log[8192] = "";
		Gl::GetProgramInfoLog(built.id, sizeof(log), nullptr, log);
		Log(LOG_ERROR) << "OpenXcom3D GPU: the program would not link: " << log;
		return false;
	}
	Gl::UseProgram(built.id);
	const char *const samplers[BufferCount] = { "uTileFlags", "uTiles", "uModelColor", "uModelBits", "uModelInfo", "uModelSides", "uSolid", "uTables", "uWaveSine",
		"uModelExposed", "uWallPaints", "uMirrors", "uBlend" };
	for (int unit = 0; unit < BufferCount; ++unit)
	{
		Gl::Uniform1i(Gl::GetUniformLocation(built.id, samplers[unit]), unit);
	}
	return true;
}

/**
 * Builds the small program that makes one picture of several rays a pixel (ResolveSource), its
 * pictures to read on the texture units after the buffers' (three for each ray: colour, depth,
 * glow). Without it there is no supersampling; all else works.
 */
bool FirstPersonGpu::State::buildResolve()
{
	const char *sources[2] = { VertexSource, ResolveSource };
	const Gl::Enum kinds[2] = { Gl::VERTEX_SHADER, Gl::FRAGMENT_SHADER };
	const Gl::UInt built = Gl::CreateProgram();
	for (int which = 0; which < 2; ++which)
	{
		const Gl::UInt shader = Gl::CreateShader(kinds[which]);
		Gl::ShaderSource(shader, 1, &sources[which], nullptr);
		Gl::CompileShader(shader);
		Gl::Int compiled = 0;
		Gl::GetShaderiv(shader, Gl::COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			char log[4096] = "";
			Gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
			Log(LOG_ERROR) << "OpenXcom3D GPU: the supersampling shader would not compile: " << log;
			return false;
		}
		Gl::AttachShader(built, shader);
		Gl::DeleteShader(shader);
	}
	Gl::LinkProgram(built);
	Gl::Int linked = 0;
	Gl::GetProgramiv(built, Gl::LINK_STATUS, &linked);
	if (!linked)
	{
		return false;
	}
	Gl::UseProgram(built);
	Gl::Uniform1i(Gl::GetUniformLocation(built, "uBlend"), Blend);
	const char *const kindNames[3] = { "uColor", "uDepth", "uTrue" };
	for (int ray = 0; ray < 4; ++ray)
	{
		for (int which = 0; which < 3; ++which)
		{
			Gl::Uniform1i(Gl::GetUniformLocation(built, (std::string(kindNames[which]) + std::to_string(ray)).c_str()), BufferCount + 1 + ray * 3 + which);
		}
	}
	resolveProgram = built;
	return true;
}

/**
 * The number the shader knows a model by, sending the model up the first time it is asked for
 * (and its own ghost with it). Per model: its colours; its masks (glass, look-only, holes,
 * lights); its sides (plan, east, south); and four numbers:
 *  flags (1 a ghost, 2 a solid ghost, 4 has lights, 8 water) | its ghost's number |
 *  glowMinZ | ceiling << 8 | min z << 16 | pulseKind << 24 | a steady UFO model's block of open faces (0 none)
 */
Uint32 FirstPersonGpu::State::modelId(const FirstPersonView::VoxelModel *model)
{
	if (!model)
	{
		return 0;
	}
	const auto known = modelIds.find(model);
	if (known != modelIds.end())
	{
		return known->second;
	}
	const Uint32 id = (Uint32)modelIds.size() + 1;
	modelIds[model] = id;
	modelColors.resize((size_t)(id + 1) * ModelVoxels);
	modelSides.resize((size_t)(id + 1) * ModelSideBytes);
	modelBits.resize((size_t)(id + 1) * MaskRows * ModelMasks);
	modelInfo.resize((size_t)(id + 1) * 4);
	const Uint32 ghostId = modelId(model->ghost); // may grow the vectors: only numbers are kept across it
	std::memcpy(&modelColors[(size_t)id * ModelVoxels], model->color, ModelVoxels);
	Uint8 *sides = &modelSides[(size_t)id * ModelSideBytes];
	std::memcpy(sides, model->plan, 256);
	std::memcpy(sides + 256, model->east, 384);
	std::memcpy(sides + 640, model->south, 384);
	Uint16 *masks = &modelBits[(size_t)id * MaskRows * ModelMasks];
	std::memcpy(masks, model->glass, MaskRows * 2);
	std::memcpy(masks + MaskRows, model->lookOnly, MaskRows * 2);
	std::memcpy(masks + MaskRows * 2, model->clear, MaskRows * 2);
	if (model->lit) std::memcpy(masks + MaskRows * 3, model->lit, MaskRows * 2);
	else std::memset(masks + MaskRows * 3, 0, MaskRows * 2);
	Uint32 *info = &modelInfo[(size_t)id * 4];
	info[0] = (model->ghostly ? 1 : 0) | (model->solidGhost ? 2 : 0) | (model->lit ? 4 : 0) | (model->water ? 8 : 0);
	info[1] = ghostId;
	info[2] = model->glowMinZ | (Uint32)model->ceiling << 8 | (Uint32)model->min[2] << 16 | (Uint32)model->pulseKind << 24;
	info[3] = 0;
	if (model->exposed)
	{
		// A steady UFO model: its voxels' open faces, in a block of their own.
		info[3] = (Uint32)(modelExposed.size() / ModelVoxels);
		modelExposed.insert(modelExposed.end(), &model->exposed[0][0][0], &model->exposed[0][0][0] + ModelVoxels);
	}
	modelsChanged = true;
	return id;
}

/**
 * The number the shader knows a solid mask by, sending it up the first time.
 */
Uint32 FirstPersonGpu::State::solidId(FirstPersonView::SolidRows solid)
{
	if (!solid)
	{
		return 0;
	}
	const auto known = solidIds.find(solid);
	if (known != solidIds.end())
	{
		return known->second;
	}
	const Uint32 id = (Uint32)solidIds.size() + 1;
	solidIds[solid] = id;
	solidRows.resize((size_t)(id + 1) * MaskRows);
	std::memcpy(&solidRows[(size_t)id * MaskRows], solid, MaskRows * 2);
	solidsChanged = true;
	return id;
}

/**
 * Packs the frame's tile snapshot into four texels (sixteen numbers) a tile:
 *  0: parts 0 and 1's model numbers | parts 2 and 3's | the box round the solid voxels (five bits
 *     each: min x, y, z, max x, y, z; bit 30: there is a box) | the solid mask's number
 *  1: light | seen << 8 | emissive << 16 | ufoGlow << 24 | the two door swings (+1, sixteen bits each) |
 *     discovered | smoke << 8 | burning << 16 | roofed << 17 | on the bottom level: the water reaches the map's edge here
 *  2: the parts' next frames: 0 and 1's model numbers | 2 and 3's | which UFO's light reaches it (+1) | its pulse phase and marks << 8 ;
 *     its west wall's paint (+1) | its north wall's << 16
 *  3: its craft's mirror (+1) | (free)
 */
void FirstPersonGpu::State::encodeTiles(const Frame &frame)
{
	const size_t count = (size_t)frame.size[0] * frame.size[1] * frame.size[2], perLevel = (size_t)frame.size[0] * frame.size[1];
	if (lastParts.size() != count)
	{
		lastParts.assign(count, { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr });
		lastSolid.assign(count, nullptr);
		tileUints.assign(count * TileUints, 0);
	}
	for (size_t index = 0; index < count; ++index)
	{
		const FirstPersonView::TileView &tile = frame.tiles[index];
		Uint32 *uints = &tileUints[index * TileUints];
		// The parts' and the mask's numbers: looked up only when the tile shows other ones than last frame.
		std::array<const FirstPersonView::VoxelModel*, 8> &parts = lastParts[index];
		if (std::memcmp(&parts[0], tile.part, sizeof(tile.part)) != 0)
		{
			for (int part = 0; part < 4; ++part) parts[part] = tile.part[part];
			uints[0] = modelId(parts[0]) | modelId(parts[1]) << 16;
			uints[1] = modelId(parts[2]) | modelId(parts[3]) << 16;
		}
		if (std::memcmp(&parts[4], tile.next, sizeof(tile.next)) != 0)
		{
			for (int part = 0; part < 4; ++part) parts[4 + part] = tile.next[part];
			uints[8] = modelId(parts[4]) | modelId(parts[5]) << 16;
			uints[9] = modelId(parts[6]) | modelId(parts[7]) << 16;
		}
		if (lastSolid[index] != tile.solid)
		{
			lastSolid[index] = tile.solid;
			uints[3] = solidId(tile.solid);
		}
		const bool box = tile.max[0] >= tile.min[0] && tile.solid;
		uints[2] = !box ? 0 : (0x40000000u | (tile.min[0] & 31) | (tile.min[1] & 31) << 5 | (tile.min[2] & 31) << 10
			| (tile.max[0] & 31) << 15 | (tile.max[1] & 31) << 20 | (Uint32)(tile.max[2] & 31) << 25);
		uints[4] = tile.light | tile.seen << 8 | tile.emissive << 16 | (Uint32)tile.ufoGlow << 24;
		uints[5] = (Uint32)(tile.swing[0] + 1) | (Uint32)(tile.swing[1] + 1) << 16;
		uints[6] = (tile.discovered ? 1 : 0) | tile.smoke << 8 | (tile.burning ? 1 << 16 : 0) | (frame.roofed[index] ? 1 << 17 : 0);
		uints[7] = index < perLevel && frame.waterAtEdge ? frame.waterAtEdge[index] : 0;
		uints[10] = (frame.ufoOfTile ? frame.ufoOfTile[index] : 0) | (frame.ufoPhaseOfTile ? frame.ufoPhaseOfTile[index] << 8 : 0);
		uints[11] = frame.wallPaintOf ? frame.wallPaintOf[index * 2] | (Uint32)frame.wallPaintOf[index * 2 + 1] << 16 : 0;
		uints[12] = frame.mirrorOfTile ? (Uint32)(frame.mirrorOfTile[index] + 1) : 0;
	}
}

/**
 * The scene's colour tables, rows of 256 bytes: 0-8 the haze's steps, 9 through glass, 10 unseen
 * terrain's tone, 11 a flare's red; 12 the scene's single colours (the sky's 32, the clouds' 8,
 * then the rest, in the order of the shader's Scene... constants); 13 which colours are all but
 * black; then fourteen rows for each UFO: its inner and outer tints, and its twelve soft tints.
 */
void FirstPersonGpu::State::encodeTables(const Frame &frame)
{
	const FirstPersonView::SceneColors &colors = *frame.colors;
	const int ufoCount = std::min(frame.ufoCount, MaxUfos);
	tables.assign((TableRows + ufoCount * UfoTableRows) * 256, 0);
	std::memcpy(&tables[0], colors.hazeTint, 9 * 256);
	std::memcpy(&tables[9 * 256], colors.glassTint, 256);
	std::memcpy(&tables[10 * 256], colors.unseenTint, 256);
	std::memcpy(&tables[11 * 256], colors.flareTint, 256);
	Uint8 *scene = &tables[12 * 256];
	std::memcpy(scene, colors.sky, 32);
	std::memcpy(scene + 32, colors.cloud, 8);
	const Uint8 rest[] = { colors.haze, colors.star, colors.moon, colors.sun, colors.tapeYellow, colors.tapeBlack, colors.post, colors.faintStar,
		colors.earth[0], colors.earth[1], colors.earth[2], colors.earth[3], colors.unexplored, colors.rock,
		frame.waterColors[0], frame.waterColors[1], frame.waterColors[2], frame.rockBase };
	std::memcpy(scene + 40, rest, sizeof(rest));
	std::memcpy(&tables[13 * 256], frame.black, 256);
	for (int ufo = 0; ufo < ufoCount; ++ufo)
	{
		Uint8 *rows = &tables[(TableRows + ufo * UfoTableRows) * 256];
		std::memcpy(rows, frame.ufoTint[ufo], 2 * 256);
		std::memcpy(rows + 2 * 256, frame.ufoSoftTint[ufo], 12 * 256);
	}
}

/**
 * What several rays a pixel blend with (setting "Supersampling"): the palette's 256 colours, red,
 * green and blue each, then for every colour of a 32 x 32 x 32 cube the palette colour nearest
 * to it (never colour 0, the transparent one). The shader mixes its rays' colours and looks the
 * mix up here. Made and sent again only when the palette is another one.
 */
void FirstPersonGpu::State::sendBlend(const Frame &frame)
{
	std::vector<Uint8> blend(768 + 32768);
	for (int color = 0; color < 256; ++color)
	{
		blend[color * 3] = frame.palette[color].r;
		blend[color * 3 + 1] = frame.palette[color].g;
		blend[color * 3 + 2] = frame.palette[color].b;
	}
	if (blendSent.size() == 768 && std::equal(blendSent.begin(), blendSent.end(), blend.begin()))
	{
		return;
	}
	for (int cell = 0; cell < 32768; ++cell)
	{
		const int red = (cell >> 10) * 8 + 4, green = ((cell >> 5) & 31) * 8 + 4, blue = (cell & 31) * 8 + 4;
		int nearest = 1, nearestDistance = 1 << 30;
		for (int color = 1; color < 256; ++color)
		{
			const int dRed = red - blend[color * 3], dGreen = green - blend[color * 3 + 1], dBlue = blue - blend[color * 3 + 2];
			const int distance = dRed * dRed + dGreen * dGreen + dBlue * dBlue;
			if (distance < nearestDistance) { nearestDistance = distance; nearest = color; }
		}
		blend[768 + cell] = (Uint8)nearest;
	}
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Blend]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)blend.size(), blend.data(), Gl::DYNAMIC_DRAW);
	blendSent.assign(blend.begin(), blend.begin() + 768);
}

/**
 * The wall paints, a texel each: the wall's model | the donor's | donorWest | openEnd << 1 |
 * kind << 8 | paintGroup << 16 | the wall's min x | min y << 8.
 */
void FirstPersonGpu::State::sendWallPaints(const Frame &frame)
{
	wallPaintTexels.assign((size_t)std::max(1, frame.wallPaintCount) * 4, 0);
	for (int index = 0; index < frame.wallPaintCount; ++index)
	{
		const WallPaint &paint = frame.wallPaints[index];
		Uint32 *texel = &wallPaintTexels[(size_t)index * 4];
		texel[0] = modelId(paint.wall);
		texel[1] = modelId(paint.donor);
		texel[2] = (paint.donorWest ? 1 : 0) | (paint.openEnd ? 2 : 0) | (Uint32)paint.kind << 8 | (Uint32)paint.paintGroup << 16;
		texel[3] = paint.wall->min[0] | (Uint32)paint.wall->min[1] << 8;
	}
	if (wallPaintTexels != wallPaintTexelsSent)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[WallPaints]);
		Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(wallPaintTexels.size() * 4), wallPaintTexels.data(), Gl::DYNAMIC_DRAW);
		wallPaintTexelsSent = wallPaintTexels;
	}
}

/**
 * The crafts' mirrors, as one row of numbers: sixteen for each mirror (lo[3], size[3], sum[2],
 * symmetric[2], then per axis where its start, pos and outer rows begin in this row), then all
 * their rows. Sent again only when the view has made its mirrors anew.
 */
void FirstPersonGpu::State::sendMirrors(const Frame &frame)
{
	std::vector<const void*> rows;
	for (int index = 0; index < frame.mirrorCount; ++index)
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			rows.push_back(frame.mirrors[index].start[axis]);
			rows.push_back(frame.mirrors[index].pos[axis]);
			rows.push_back(frame.mirrors[index].outer[axis]);
			rows.push_back((const void*)(frame.mirrors[index].posCount[axis] + frame.mirrors[index].startCount[axis]));
		}
	}
	if (rows == mirrorsSent && !mirrorNumbers.empty())
	{
		return;
	}
	mirrorsSent = rows;
	mirrorNumbers.assign((size_t)std::max(1, frame.mirrorCount) * 16, 0);
	for (int index = 0; index < frame.mirrorCount; ++index)
	{
		const Mirror &mirror = frame.mirrors[index];
		Sint32 header[16] = { mirror.lo[0], mirror.lo[1], mirror.lo[2], mirror.size[0], mirror.size[1], mirror.size[2], mirror.sum[0], mirror.sum[1],
			mirror.symmetric[0] ? 1 : 0, mirror.symmetric[1] ? 1 : 0 };
		for (int axis = 0; axis < 2; ++axis)
		{
			header[10 + axis * 3] = (Sint32)mirrorNumbers.size();
			mirrorNumbers.insert(mirrorNumbers.end(), mirror.start[axis], mirror.start[axis] + mirror.startCount[axis]);
			header[11 + axis * 3] = (Sint32)mirrorNumbers.size();
			mirrorNumbers.insert(mirrorNumbers.end(), mirror.pos[axis], mirror.pos[axis] + mirror.posCount[axis]);
			header[12 + axis * 3] = (Sint32)mirrorNumbers.size();
			mirrorNumbers.insert(mirrorNumbers.end(), mirror.outer[axis], mirror.outer[axis] + mirror.outerCount[axis]);
		}
		std::copy(header, header + 16, mirrorNumbers.begin() + (size_t)index * 16);
	}
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Mirrors]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(mirrorNumbers.size() * 4), mirrorNumbers.data(), Gl::DYNAMIC_DRAW);
}

/**
 * Sends the models up again, whole, when any was added.
 */
void FirstPersonGpu::State::sendModels()
{
	if (!modelsChanged)
	{
		return;
	}
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[ModelColors]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)modelColors.size(), modelColors.data(), Gl::DYNAMIC_DRAW);
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[ModelSides]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)modelSides.size(), modelSides.data(), Gl::DYNAMIC_DRAW);
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Exposed]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)modelExposed.size(), modelExposed.data(), Gl::DYNAMIC_DRAW);
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[ModelBits]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(modelBits.size() * 2), modelBits.data(), Gl::DYNAMIC_DRAW);
	Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[ModelInfo]);
	Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(modelInfo.size() * 4), modelInfo.data(), Gl::DYNAMIC_DRAW);
	modelsChanged = false;
}

/**
 * The camera, the settings and the scene's numbers, as the shader's uniforms.
 */
void FirstPersonGpu::State::setScene(const Frame &frame)
{
	const FirstPersonView::SceneColors &colors = *frame.colors;
	Gl::Uniform3i(location("uSize"), frame.size[0], frame.size[1], frame.size[2]);
	Gl::Uniform1f(location("uMaxDistance"), frame.maxDistance);
	Gl::Uniform3f(location("uEye"), frame.eye[0], frame.eye[1], frame.eye[2]);
	Gl::Uniform3f(location("uForward"), frame.forward[0], frame.forward[1], frame.forward[2]);
	Gl::Uniform3f(location("uRight"), frame.right[0], frame.right[1], frame.right[2]);
	Gl::Uniform3f(location("uUp"), frame.up[0], frame.up[1], frame.up[2]);
	Gl::Uniform2f(location("uHalfPlane"), frame.halfPlaneWidth, frame.halfPlaneHeight);
	Gl::Uniform2i(location("uViewSize"), frame.width, frame.height);
	Gl::Uniform1i(location("uPixelSize"), frame.pixelSize);
	Gl::Uniform1f(location("uGhostScale"), frame.ghostScale);
	Gl::Uniform1i(location("uSeeThroughCrops"), frame.seeThroughCrops ? 1 : 0);
	Gl::Uniform1i(location("uSky"), frame.sky);
	Gl::Uniform1i(location("uUnderwater"), colors.underwater ? 1 : 0);
	Gl::Uniform1i(location("uOvercast"), colors.overcast ? 1 : 0);
	Gl::Uniform1i(location("uEnclosed"), frame.enclosed ? 1 : 0);
	Gl::Uniform1i(location("uSurfaceLevel"), frame.surfaceLevel);
	Gl::Uniform1i(location("uAmbient"), colors.ambient);
	Gl::Uniform1i(location("uFogOfWar"), frame.fogOfWar);
	Gl::Uniform1i(location("uUnseenTerrain"), frame.unseenTerrain);
	Gl::Uniform1i(location("uHaze"), frame.haze);
	Gl::Uniform1i(location("uShadows"), frame.shadows ? 1 : 0);
	Gl::Uniform1i(location("uSmoothLighting"), frame.smoothLighting ? 1 : 0);
	Gl::Uniform1i(location("uDiscreetEdge"), frame.discreetEdge ? 1 : 0);
	Gl::Uniform1i(location("uSoftShadows"), frame.softShadows ? 1 : 0);
	Gl::Uniform1i(location("uAllLightShadows"), frame.allLightShadows ? 1 : 0);
	Gl::Uniform1i(location("uAmbientOcclusion"), frame.ambientOcclusion ? 1 : 0);
	Gl::Uniform1i(location("uSmoothLight"), frame.smoothLight && frame.palette ? 1 : 0);
	Gl::Uniform1i(location("uGroundModel"), (Gl::Int)modelId(frame.groundModel));
	Gl::Uniform1i(location("uWaterModel"), (Gl::Int)modelId(frame.waterModel));
	Gl::Uniform1f(location("uNight"), colors.night);
	Gl::Uniform1f(location("uCloudTime"), colors.cloudTime);
	Gl::Uniform1f(location("uAnimationBlend"), frame.animationBlend);
	Gl::Uniform1f(location("uSightView"), frame.sightView);
	Gl::Uniform1f(location("uSightMod"), frame.sightMod);
	Gl::Uniform1fv(location("uFaceShade"), 6, colors.faceShade);
	Gl::Uniform1fv(location("uIndoorFaceShade"), 6, colors.indoorFaceShade);
	Gl::Uniform3f(location("uSunDir"), colors.sunDir[0], colors.sunDir[1], colors.sunDir[2]);
	Gl::Uniform3f(location("uMoonDir"), colors.moonDir[0], colors.moonDir[1], colors.moonDir[2]);
	Gl::Uniform1fv(location("uWaveSteps"), 18, frame.waveSteps);
	Gl::Uniform4fv(location("uLamps"), 16, &frame.lamps[0][0]);
	Gl::Uniform1i(location("uLampCount"), frame.lampCount);
	Gl::Uniform1i(location("uFlashlight"), frame.flashlight);
	Gl::Uniform3f(location("uFlashlightPos"), frame.flashlightPos[0], frame.flashlightPos[1], frame.flashlightPos[2]);
	Gl::Uniform3f(location("uForwardInVoxels"), frame.forwardInVoxels[0], frame.forwardInVoxels[1], frame.forwardInVoxels[2]);
	Gl::Uniform4fv(location("uFlares"), 8, &frame.flares[0][0]);
	Gl::Uniform1fv(location("uFlareFlicker"), 8, frame.flareFlicker);
	Gl::Uniform1i(location("uFlareCount"), frame.flareCount);
	Gl::Uniform1f(location("uMuzzleFlashLight"), frame.muzzleFlashLight);
	Gl::Uniform1i(location("uUfoLightsOn"), frame.ufoLightsOn ? 1 : 0);
	Gl::Uniform1i(location("uWallPaint"), frame.wallPaintOf ? frame.wallPaint : 0);
	Gl::Uniform1fv(location("uUfoPulse"), 32, frame.ufoPulse);
	Gl::Uniform1f(location("uUfoBreath"), frame.ufoBreath);
	Gl::Uniform1fv(location("uUfoFaceShade"), 64, frame.ufoFaceShade);
	Gl::Uniform3f(location("uMuzzleFlashPos"), frame.muzzleFlashPos[0], frame.muzzleFlashPos[1], frame.muzzleFlashPos[2]);
	float swingA[MaxSwings * 4] = {}, swingB[MaxSwings * 4] = {};
	for (int swing = 0; swing < std::min(frame.swingCount, MaxSwings); ++swing)
	{
		const FirstPersonView::DoorSwing &door = frame.swings[swing];
		const float a[4] = { door.hinge[0], door.hinge[1], door.dir[0], door.dir[1] };
		const float b[4] = { door.z0, door.z1, (float)modelId(door.model), door.westWall ? 1.0f : 0.0f };
		std::copy(a, a + 4, swingA + swing * 4);
		std::copy(b, b + 4, swingB + swing * 4);
	}
	Gl::Uniform4fv(location("uSwingA"), MaxSwings, swingA);
	Gl::Uniform4fv(location("uSwingB"), MaxSwings, swingB);
}

/**
 * Makes the pictures the rays are drawn into (colour, depth, glow, and for the check what each
 * ray met) of the size asked the ones drawn into now; made the first time a size is asked for,
 * then kept (a few sizes: when there are many, all are made anew).
 */
bool FirstPersonGpu::State::setTargets(int width, int height, bool results, int layer)
{
	if (targets.size() > 40 && layer == 0)
	{
		for (auto &entry : targets)
		{
			Gl::DeleteTextures(5, entry.second.textures);
			Gl::DeleteFramebuffers(1, &entry.second.framebuffer);
		}
		targets.clear();
	}
	Targets &sized = targets[{ width, height, layer }];
	if (!sized.framebuffer || (results && !sized.results))
	{
		if (sized.framebuffer)
		{
			Gl::DeleteTextures(5, sized.textures);
			Gl::DeleteFramebuffers(1, &sized.framebuffer);
		}
		Gl::GenFramebuffers(1, &sized.framebuffer);
		Gl::BindFramebuffer(Gl::FRAMEBUFFER, sized.framebuffer);
		Gl::GenTextures(5, sized.textures);
		const Gl::Enum internal[5] = { Gl::R8UI, Gl::R32F, Gl::R8UI, Gl::RGBA8, Gl::RGBA32UI }, format[5] = { Gl::RED_INTEGER, Gl::RED, Gl::RED_INTEGER, Gl::RGBA, Gl::RGBA_INTEGER },
			type[5] = { Gl::UNSIGNED_BYTE, Gl::FLOAT, Gl::UNSIGNED_BYTE, Gl::UNSIGNED_BYTE, Gl::UNSIGNED_INT };
		Gl::ActiveTexture(Gl::TEXTURE0 + BufferCount);
		for (int which = 0; which < (results ? 5 : 4); ++which)
		{
			Gl::BindTexture(Gl::TEXTURE_2D, sized.textures[which]);
			Gl::TexImage2D(Gl::TEXTURE_2D, 0, (Gl::Int)internal[which], width, height, 0, format[which], type[which], nullptr);
			Gl::TexParameteri(Gl::TEXTURE_2D, Gl::TEXTURE_MIN_FILTER, (Gl::Int)Gl::NEAREST);
			Gl::TexParameteri(Gl::TEXTURE_2D, Gl::TEXTURE_MAG_FILTER, (Gl::Int)Gl::NEAREST);
			Gl::FramebufferTexture2D(Gl::FRAMEBUFFER, Gl::COLOR_ATTACHMENT0 + which, Gl::TEXTURE_2D, sized.textures[which], 0);
		}
		sized.results = results;
		if (Gl::CheckFramebufferStatus(Gl::FRAMEBUFFER) != Gl::FRAMEBUFFER_COMPLETE)
		{
			return false;
		}
	}
	Gl::BindFramebuffer(Gl::FRAMEBUFFER, sized.framebuffer);
	const Gl::Enum drawn[5] = { Gl::COLOR_ATTACHMENT0, Gl::COLOR_ATTACHMENT0 + 1, Gl::COLOR_ATTACHMENT0 + 2, Gl::COLOR_ATTACHMENT0 + 3, Gl::COLOR_ATTACHMENT0 + 4 };
	Gl::DrawBuffers(results ? 5 : 4, drawn);
	return true;
}

/**
 * One frame on the card: sends up what changed, draws the rays, and reads the picture back into
 * the view's buffers.
 */
bool FirstPersonGpu::State::draw(const Frame &frame, const Output &output, Timing *timing)
{
	// More than the shader takes at once (doors swinging, UFOs with their own light, models: a part's
	// number is sixteen bits): this frame is the CPU's. None of the games' maps comes near.
	overLimit = frame.swingCount > MaxSwings || frame.ufoCount > MaxUfos || modelIds.size() >= 0xFFF0;
	if (overLimit)
	{
		if (!limitLogged)
		{
			limitLogged = true;
			Log(LOG_INFO) << "OpenXcom3D GPU: a frame beyond the card's limits (" << frame.swingCount << " doors swinging, " << frame.ufoCount << " UFOs, "
				<< modelIds.size() << " models): the CPU draws such frames";
		}
		return false;
	}
	// The world: this frame's tiles and colours, the camera and the scene, and any model or mask not sent before.
	// What is the same as last frame isn't sent again (in a turn-based game, most frames most of it).
	auto start = std::chrono::steady_clock::now();
	program = &programs[output.results ? 1 : 0];
	Gl::UseProgram(program->id);
	const size_t tileCount = (size_t)frame.size[0] * frame.size[1] * frame.size[2];
	encodeTiles(frame);
	encodeTables(frame);
	setScene(frame);
	sendWallPaints(frame);
	sendMirrors(frame);
	if (modelColors.size() > (size_t)maxBufferTexels || tileUints.size() / 4 > (size_t)maxBufferTexels)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: the world is bigger than this card's buffers take";
		return false;
	}
	if (tileFlagsSent.size() != tileCount || std::memcmp(tileFlagsSent.data(), frame.tileFlags, tileCount) != 0)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[TileFlags]);
		Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)tileCount, frame.tileFlags, Gl::DYNAMIC_DRAW);
		tileFlagsSent.assign(frame.tileFlags, frame.tileFlags + tileCount);
	}
	if (tileUints != tileUintsSent)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Tiles]);
		Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(tileUints.size() * 4), tileUints.data(), Gl::DYNAMIC_DRAW);
		tileUintsSent = tileUints;
	}
	if (tables != tablesSent)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Tables]);
		Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)tables.size(), tables.data(), Gl::DYNAMIC_DRAW);
		tablesSent = tables;
	}
	sendModels();
	if (solidsChanged)
	{
		Gl::BindBuffer(Gl::ARRAY_BUFFER, buffers[Solids]);
		Gl::BufferData(Gl::ARRAY_BUFFER, (Gl::SizeiPtr)(solidRows.size() * 2), solidRows.data(), Gl::DYNAMIC_DRAW);
		solidsChanged = false;
	}
	const int blockColumns = (frame.width + frame.pixelSize - 1) / frame.pixelSize, blockRows = (frame.height + frame.pixelSize - 1) / frame.pixelSize;
	if (!setTargets(blockColumns, blockRows, output.results != nullptr))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: this card can't draw into the pictures the rays need";
		return false;
	}
	if (timing)
	{
		Gl::Finish();
		timing->upload = millisecondsSince(start);
		start = std::chrono::steady_clock::now();
	}

	// The rays: one a pixel through its middle; or (setting "Supersampling") two or four, each set drawn into
	// pictures of its own, and the frame's picture made of them (see ResolveSource). The check against the
	// CPU's rays is of one ray a pixel.
	if (frame.palette) sendBlend(frame); // (the palette: for the full-colour picture and for several rays a pixel)
	Gl::Viewport(0, 0, blockColumns, blockRows);
	const int samples = resolveProgram && frame.palette && !output.results ? (frame.samples >= 4 ? 4 : frame.samples >= 2 ? 2 : 1) : 1;
	if (samples == 1)
	{
		Gl::Uniform2f(location("uRayOffset"), 0.0f, 0.0f);
		Gl::DrawArrays(Gl::TRIANGLES, 0, 3);
	}
	else
	{
		// Where a pixel's rays go, from its middle: two on a diagonal, or four on a turned square.
		static const float twoRays[2][2] = { { -0.25f, -0.25f }, { 0.25f, 0.25f } };
		static const float fourRays[4][2] = { { -0.125f, -0.375f }, { 0.375f, -0.125f }, { -0.375f, 0.125f }, { 0.125f, 0.375f } };
		for (int ray = 0; ray < samples; ++ray)
		{
			if (!setTargets(blockColumns, blockRows, false, 1 + ray))
			{
				Log(LOG_WARNING) << "OpenXcom3D GPU: this card can't draw into the pictures several rays a pixel need";
				return false;
			}
			const float *offset = samples == 2 ? twoRays[ray] : fourRays[ray];
			Gl::Uniform2f(location("uRayOffset"), offset[0], offset[1]);
			Gl::DrawArrays(Gl::TRIANGLES, 0, 3);
		}
		setTargets(blockColumns, blockRows, false);
		Gl::UseProgram(resolveProgram);
		Gl::Uniform1i(Gl::GetUniformLocation(resolveProgram, "uSamples"), samples);
		for (int ray = 0; ray < samples; ++ray)
		{
			const Targets &drawn = targets[{ blockColumns, blockRows, 1 + ray }];
			for (int which = 0; which < 3; ++which) // (colour, depth, and the full colour with the glow in it)
			{
				Gl::ActiveTexture(Gl::TEXTURE0 + BufferCount + 1 + ray * 3 + which);
				Gl::BindTexture(Gl::TEXTURE_2D, drawn.textures[which == 2 ? 3 : which]);
			}
		}
		Gl::ActiveTexture(Gl::TEXTURE0 + BufferCount);
		Gl::DrawArrays(Gl::TRIANGLES, 0, 3);
		Gl::UseProgram(program->id);
	}
	if (timing)
	{
		Gl::Finish();
		timing->draw = millisecondsSince(start);
		start = std::chrono::steady_clock::now();
	}

	// The picture, back to the view: straight into its buffers with a ray a pixel; else a ray each
	// first, then spread over their blocks of pixels as the CPU's rays fill theirs.
	const bool blocks = frame.pixelSize > 1 && !output.perRay;
	if (blocks)
	{
		blockColors.resize((size_t)blockColumns * blockRows);
		blockGlow.resize(blockColors.size());
		blockDepths.resize(blockColors.size());
	}
	Gl::ReadBuffer(Gl::COLOR_ATTACHMENT0);
	Gl::PixelStorei(Gl::PACK_ROW_LENGTH, blocks ? 0 : output.colorPitch);
	Gl::ReadPixels(0, 0, blockColumns, blockRows, Gl::RED_INTEGER, Gl::UNSIGNED_BYTE, blocks ? blockColors.data() : output.color);
	Gl::PixelStorei(Gl::PACK_ROW_LENGTH, 0);
	Gl::ReadBuffer(Gl::COLOR_ATTACHMENT0 + 1);
	Gl::ReadPixels(0, 0, blockColumns, blockRows, Gl::RED, Gl::FLOAT, blocks ? blockDepths.data() : output.depth);
	Gl::ReadBuffer(Gl::COLOR_ATTACHMENT0 + 2);
	Gl::ReadPixels(0, 0, blockColumns, blockRows, Gl::RED_INTEGER, Gl::UNSIGNED_BYTE, blocks ? blockGlow.data() : output.glow);
	if (output.results)
	{
		Gl::ReadBuffer(Gl::COLOR_ATTACHMENT0 + 4);
		Gl::ReadPixels(0, 0, blockColumns, blockRows, Gl::RGBA_INTEGER, Gl::UNSIGNED_INT, output.results);
	}
	if (output.trueColor && !blocks)
	{
		// The full-colour picture (blue, green, red, the glow: a 0xGGRRGGBB number a pixel as this machine reads it).
		Gl::ReadBuffer(Gl::COLOR_ATTACHMENT0 + 3);
		Gl::ReadPixels(0, 0, blockColumns, blockRows, Gl::BGRA, Gl::UNSIGNED_INT_8_8_8_8_REV, output.trueColor);
	}
	if (blocks)
	{
		for (int pixelY = 0; pixelY < frame.height; ++pixelY)
		{
			const size_t blockRow = (size_t)(pixelY / frame.pixelSize) * blockColumns;
			Uint8 *colorRow = output.color + (size_t)pixelY * output.colorPitch, *glowRow = output.glow + (size_t)pixelY * frame.width;
			float *depthRow = output.depth + (size_t)pixelY * frame.width;
			for (int pixelX = 0; pixelX < frame.width; ++pixelX)
			{
				const size_t block = blockRow + pixelX / frame.pixelSize;
				colorRow[pixelX] = blockColors[block];
				depthRow[pixelX] = blockDepths[block];
				glowRow[pixelX] = blockGlow[block];
			}
		}
	}
	if (timing)
	{
		timing->read = millisecondsSince(start);
	}
	const Gl::Enum error = Gl::GetError();
	if (error != Gl::NoError)
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: OpenGL error " << std::hex << error;
		return false;
	}
	return true;
}

// ---- The renderer, as the view sees it ----

FirstPersonGpu::FirstPersonGpu() : _state(new State)
{
}

/**
 * Opens the renderer: its thread, its context, its shader.
 */
std::unique_ptr<FirstPersonGpu> FirstPersonGpu::open(int device)
{
	std::unique_ptr<FirstPersonGpu> gpu(new FirstPersonGpu);
	State &state = *gpu->_state;
	if (!state.thread.start(&state, &State::threadLoop))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no thread for the renderer";
		return nullptr;
	}
	bool started = false;
	state.run([&] { started = state.start(device); });
	if (!started)
	{
		return nullptr;
	}
	Log(LOG_INFO) << "OpenXcom3D GPU: the 3D view's rays on " << state.context.name();
	return gpu;
}

std::unique_ptr<FirstPersonGpu> FirstPersonGpu::openAny()
{
	for (int device = 0; device < 16; ++device)
	{
		if (std::unique_ptr<FirstPersonGpu> gpu = open(device))
		{
			return gpu;
		}
	}
	Log(LOG_INFO) << "OpenXcom3D GPU: no graphics card to draw on: the CPU draws";
	return nullptr;
}

/**
 * Stops the renderer's thread, which closes the context on its way out.
 */
FirstPersonGpu::~FirstPersonGpu()
{
	{
		std::lock_guard<std::mutex> lock(_state->mutex);
		_state->stop = true;
	}
	_state->wake.notify_one();
	_state->thread.join();
}

const std::string &FirstPersonGpu::name() const
{
	return _state->context.name();
}

void FirstPersonGpu::forgetModels()
{
	State &state = *_state;
	state.run([&] {
		state.modelIds.clear();
		state.solidIds.clear();
		state.modelColors.assign(ModelVoxels, 0);
		state.modelSides.assign(ModelSideBytes, 0);
		state.modelExposed.assign(ModelVoxels, 0);
		state.mirrorsSent.clear();
		state.mirrorNumbers.clear();
		state.modelBits.assign(MaskRows * ModelMasks, 0);
		state.modelInfo.assign(4, 0);
		state.solidRows.assign(MaskRows, 0);
		state.lastParts.clear();
		state.lastSolid.clear();
		state.modelsChanged = state.solidsChanged = true;
	});
}

bool FirstPersonGpu::castRays(const Frame &frame, const Output &output, Timing *timing)
{
	State &state = *_state;
	if (state.failed)
	{
		return false;
	}
	bool drawn = false;
	state.run([&] { drawn = state.draw(frame, output, timing); });
	if (!drawn && !state.overLimit)
	{
		state.failed = true;
		Log(LOG_WARNING) << "OpenXcom3D GPU: the graphics card failed: the CPU draws from now on";
	}
	return drawn;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
