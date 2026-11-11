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
 * FirstPersonGpuWindows.cpp: the GPU renderer's hidden OpenGL context on Windows (see
 * GpuContext in FirstPersonGpuGl.h; Linux's and macOS's are in FirstPersonGpu.cpp).
 *
 * Windows gives an OpenGL context only to a window, so the renderer makes one of its own, one
 * pixel big and never shown, on its own thread; the rays are drawn into our own textures, never
 * into it. A file of its own because it needs windows.h, whose many macros are best kept away
 * from everything else.
 */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "FirstPersonGpuGl.h"
#include "../Engine/Logger.h"

// Laptops with two graphics chips run a program on the built-in one unless it says otherwise:
// these two names, exported from the program, ask NVIDIA's and AMD's drivers for the fast one.
extern "C"
{
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace OpenXcom
{

namespace
{

HMODULE openGlLibrary = nullptr; // opengl32.dll

/// An OpenGL function by name: the driver's (wglGetProcAddress), or one of OpenGL 1.1's, which only opengl32.dll itself has.
void *windowsFunction(const char *name)
{
	void *function = (void *)wglGetProcAddress(name);
	const INT_PTR value = (INT_PTR)function;
	if (value >= -1 && value <= 3) // (drivers answer 0, 1, 2, 3 or -1 for "none")
	{
		function = (void *)GetProcAddress(openGlLibrary, name);
	}
	return function;
}

}

/**
 * Opens a hidden OpenGL 3.3 core context through WGL: a window of our own, never shown, a
 * plain context on it to ask the driver for the function that makes a core one, then that one.
 * Windows has one renderer to offer a program (the driver's choice): device 0.
 */
bool GpuContext::create(int device)
{
	if (device > 0)
	{
		return false; // no such device
	}
	openGlLibrary = LoadLibraryA("opengl32.dll");
	if (!openGlLibrary)
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: no opengl32.dll on this machine";
		return false;
	}
	_library = openGlLibrary;
	_getProcAddress = windowsFunction;

	// The hidden window and its pixel format.
	WNDCLASSA windowClass = {};
	windowClass.lpfnWndProc = DefWindowProcA;
	windowClass.hInstance = GetModuleHandleA(nullptr);
	windowClass.lpszClassName = "OpenXcom3DGpu";
	windowClass.style = CS_OWNDC;
	RegisterClassA(&windowClass); // (fails harmlessly when it is registered already)
	const HWND window = CreateWindowExA(0, "OpenXcom3DGpu", "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
	const HDC deviceContext = window ? GetDC(window) : nullptr;
	_window = window;
	_deviceContext = deviceContext;
	PIXELFORMATDESCRIPTOR format = {};
	format.nSize = sizeof(format);
	format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
	format.iPixelType = PFD_TYPE_RGBA;
	format.cColorBits = 24;
	const int chosen = deviceContext ? ChoosePixelFormat(deviceContext, &format) : 0;
	if (!chosen || !SetPixelFormat(deviceContext, chosen, &format))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no window for an OpenGL context (error " << GetLastError() << ")";
		return false;
	}

	// A plain context first: only with one current does the driver hand out the function that makes a core one.
	const HGLRC plain = wglCreateContext(deviceContext);
	if (!plain || !wglMakeCurrent(deviceContext, plain))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no OpenGL on this machine (error " << GetLastError() << ")";
		return false;
	}
	typedef HGLRC (WINAPI *CreateContextAttribs)(HDC, HGLRC, const int *);
	const auto createContextAttribs = (CreateContextAttribs)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
	const int ContextMajor = 0x2091, ContextMinor = 0x2092, ContextProfile = 0x9126, CoreProfile = 1;
	const int wanted[] = { ContextMajor, 3, ContextMinor, 3, ContextProfile, CoreProfile, 0 };
	const HGLRC core = createContextAttribs ? createContextAttribs(deviceContext, nullptr, wanted) : nullptr;
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(plain);
	if (!core || !wglMakeCurrent(deviceContext, core))
	{
		Log(LOG_WARNING) << "OpenXcom3D GPU: no OpenGL 3.3 context (this machine's driver is too old, or Windows' own)";
		return false;
	}
	_context = core;
	if (!loadFunctions())
	{
		return false;
	}
	_name = std::string((const char *)Gl::GetString(Gl::RENDERER)) + " (OpenGL " + (const char *)Gl::GetString(Gl::VERSION) + ")";
	return true;
}

/**
 * Closes the context and its hidden window.
 */
void GpuContext::destroy()
{
	if (!_library)
	{
		return;
	}
	wglMakeCurrent(nullptr, nullptr);
	if (_context) wglDeleteContext((HGLRC)_context);
	if (_window && _deviceContext) ReleaseDC((HWND)_window, (HDC)_deviceContext);
	if (_window) DestroyWindow((HWND)_window);
	_context = _window = _deviceContext = nullptr;
	_library = nullptr; // (opengl32.dll stays loaded)
}

bool GpuContext::software() const
{
	// Windows' own OpenGL 1.1 ("GDI Generic") never gets this far; Mesa's software drivers might.
	for (const char *name : { "llvmpipe", "softpipe", "SwiftShader", "GDI Generic", "Software" })
	{
		if (_name.find(name) != std::string::npos) return true;
	}
	return false;
}

}

#endif
