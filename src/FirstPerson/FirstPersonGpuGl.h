#pragma once
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
 * FirstPersonGpuGl.h: the little of OpenGL the GPU renderer uses, declared here by hand and
 * looked up at run time, so the game needs no OpenGL headers or libraries to build and starts
 * the same on a machine without a graphics card ("Runs anywhere": the CPU renderer stays).
 *
 * The context is the renderer's own, hidden and without a window, apart from the game's display
 * (which draws through SDL, with or without OXCE's own OpenGL output): see GpuContext.
 * Only FirstPersonGpu.cpp includes this.
 */
#include <cstddef>
#include <cstdint>
#include <string>

namespace OpenXcom
{

namespace Gl
{

typedef unsigned int Enum;
typedef unsigned int UInt;
typedef int Int;
typedef int Sizei;
typedef float Float;
typedef char Char;
typedef unsigned char Boolean;
typedef std::ptrdiff_t SizeiPtr;
typedef std::ptrdiff_t IntPtr;

// The constants used (their values are OpenGL's own).
const Enum VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02;
const Enum NoError = 0; // (GL_NO_ERROR: not spelt so, Windows has a macro of that name)
const Enum TEXTURE_2D = 0x0DE1, TEXTURE_BUFFER = 0x8C2A, TEXTURE0 = 0x84C0;
const Enum TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, NEAREST = 0x2600;
const Enum TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F;
const Enum R8UI = 0x8232, R16UI = 0x8234, R32I = 0x8235, R32F = 0x822E, RGBA32UI = 0x8D70, RGBA32F = 0x8814;
const Enum RED = 0x1903, RED_INTEGER = 0x8D94, RGBA_INTEGER = 0x8D99, RGBA = 0x1908, RGBA8 = 0x8058, BGRA = 0x80E1, UNSIGNED_INT_8_8_8_8_REV = 0x8367;
const Enum UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT = 0x1403, UNSIGNED_INT = 0x1405, FLOAT = 0x1406;
const Enum ARRAY_BUFFER = 0x8892, STREAM_DRAW = 0x88E0, DYNAMIC_DRAW = 0x88E8;
const Enum PIXEL_PACK_BUFFER = 0x88EB, STREAM_READ = 0x88E1, READ_ONLY = 0x88B8;
const Enum FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5;
const Enum FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84;
const Enum TRIANGLES = 0x0004, DEPTH_TEST = 0x0B71, BLEND = 0x0BE2, DITHER = 0x0BD0;
const Enum PACK_ALIGNMENT = 0x0D05, UNPACK_ALIGNMENT = 0x0CF5, PACK_ROW_LENGTH = 0x0D02;
const Enum MAX_TEXTURE_BUFFER_SIZE = 0x8C2B;
const Enum TIME_ELAPSED = 0x88BF, QUERY_RESULT = 0x8866;
const Enum IMPLEMENTATION_COLOR_READ_TYPE = 0x8B9A, IMPLEMENTATION_COLOR_READ_FORMAT = 0x8B9B;

// The functions used: one line each, as OpenGL declares them. GL_FUNCTIONS(X) lists them for the
// declarations below and for the loader (GpuContext::loadFunctions).
#if defined(_WIN32)
#define GL_CALL __stdcall
#else
#define GL_CALL
#endif
#define GL_FUNCTIONS(X) \
	X(const unsigned char *, GetString, (Enum name)) \
	X(void, GetIntegerv, (Enum name, Int *data)) \
	X(Enum, GetError, (void)) \
	X(void, Finish, (void)) \
	X(void, Flush, (void)) \
	X(void, Disable, (Enum capability)) \
	X(void, Viewport, (Int x, Int y, Sizei width, Sizei height)) \
	X(void, PixelStorei, (Enum name, Int value)) \
	X(void, GenBuffers, (Sizei count, UInt *buffers)) \
	X(void, DeleteBuffers, (Sizei count, const UInt *buffers)) \
	X(void, BindBuffer, (Enum target, UInt buffer)) \
	X(void, BufferData, (Enum target, SizeiPtr size, const void *data, Enum usage)) \
	X(void, BufferSubData, (Enum target, IntPtr offset, SizeiPtr size, const void *data)) \
	X(void, GenTextures, (Sizei count, UInt *textures)) \
	X(void, DeleteTextures, (Sizei count, const UInt *textures)) \
	X(void, BindTexture, (Enum target, UInt texture)) \
	X(void, ActiveTexture, (Enum unit)) \
	X(void, TexBuffer, (Enum target, Enum format, UInt buffer)) \
	X(void, TexImage2D, (Enum target, Int level, Int internalFormat, Sizei width, Sizei height, Int border, Enum format, Enum type, const void *pixels)) \
	X(void, TexSubImage2D, (Enum target, Int level, Int x, Int y, Sizei width, Sizei height, Enum format, Enum type, const void *pixels)) \
	X(void, TexParameteri, (Enum target, Enum name, Int value)) \
	X(void, GenFramebuffers, (Sizei count, UInt *framebuffers)) \
	X(void, DeleteFramebuffers, (Sizei count, const UInt *framebuffers)) \
	X(void, BindFramebuffer, (Enum target, UInt framebuffer)) \
	X(void, FramebufferTexture2D, (Enum target, Enum attachment, Enum textureTarget, UInt texture, Int level)) \
	X(Enum, CheckFramebufferStatus, (Enum target)) \
	X(void, DrawBuffers, (Sizei count, const Enum *buffers)) \
	X(void, ReadBuffer, (Enum buffer)) \
	X(void, ReadPixels, (Int x, Int y, Sizei width, Sizei height, Enum format, Enum type, void *pixels)) \
	X(UInt, CreateShader, (Enum type)) \
	X(void, ShaderSource, (UInt shader, Sizei count, const Char *const *strings, const Int *lengths)) \
	X(void, CompileShader, (UInt shader)) \
	X(void, GetShaderiv, (UInt shader, Enum name, Int *value)) \
	X(void, GetShaderInfoLog, (UInt shader, Sizei size, Sizei *length, Char *log)) \
	X(void, DeleteShader, (UInt shader)) \
	X(UInt, CreateProgram, (void)) \
	X(void, AttachShader, (UInt program, UInt shader)) \
	X(void, LinkProgram, (UInt program)) \
	X(void, GetProgramiv, (UInt program, Enum name, Int *value)) \
	X(void, GetProgramInfoLog, (UInt program, Sizei size, Sizei *length, Char *log)) \
	X(void, DeleteProgram, (UInt program)) \
	X(void, UseProgram, (UInt program)) \
	X(Int, GetUniformLocation, (UInt program, const Char *name)) \
	X(void, Uniform1i, (Int location, Int a)) \
	X(void, Uniform2i, (Int location, Int a, Int b)) \
	X(void, Uniform3i, (Int location, Int a, Int b, Int c)) \
	X(void, Uniform1f, (Int location, Float a)) \
	X(void, Uniform2f, (Int location, Float a, Float b)) \
	X(void, Uniform3f, (Int location, Float a, Float b, Float c)) \
	X(void, Uniform1fv, (Int location, Sizei count, const Float *values)) \
	X(void, Uniform4fv, (Int location, Sizei count, const Float *values)) \
	X(void, GenVertexArrays, (Sizei count, UInt *arrays)) \
	X(void, DeleteVertexArrays, (Sizei count, const UInt *arrays)) \
	X(void, BindVertexArray, (UInt array)) \
	X(void, DrawArrays, (Enum mode, Int first, Sizei count))

// Each function as a pointer, filled in by GpuContext::loadFunctions: Gl::Finish(), Gl::BindBuffer(...).
#define GL_DECLARE(result, name, arguments) extern result (GL_CALL *name) arguments;
GL_FUNCTIONS(GL_DECLARE)
#undef GL_DECLARE

}

/**
 * The GPU renderer's own OpenGL context: hidden, with no window, on the graphics card asked for.
 * On Linux it comes from EGL (libEGL.so.1, opened at run time), on a device picked by number
 * where the machine has more than one (a laptop's built-in graphics and its graphics card);
 * on macOS from CGL (the OpenGL framework, opened at run time); on Windows from WGL, on a
 * hidden window (FirstPersonGpuWindows.cpp).
 * It belongs to the thread that creates it: make it, use it and destroy it on that one thread.
 */
class GpuContext
{
public:
	/// Opens the context (OpenGL 3.3, core; or with `es`, OpenGL ES 3.2, on Linux's EGL only) on a device by number.
	/// False where there is none to be had (no such device: silently; else the reason is in the log).
	bool create(int device, bool es = false);
	/// Whether it is an OpenGL ES context (the shaders and the read-back differ: see FirstPersonGpu.cpp).
	bool es() const { return _es; }
	/// Whether the context draws on the CPU (a software OpenGL such as Mesa's llvmpipe): no use to us.
	bool software() const;
	/// Closes it.
	void destroy();
	/// The graphics card's name, as its driver gives it.
	const std::string &name() const { return _name; }
	~GpuContext() { destroy(); }
private:
	bool loadFunctions();
	void *_window = nullptr;   // Windows: the hidden window the context needs, and its device context
	void *_deviceContext = nullptr;
	void *_library = nullptr;  // libEGL, or macOS's OpenGL framework
	void *_display = nullptr;
	void *_context = nullptr;
	void *_surface = nullptr;
	void *(*_getProcAddress)(const char *) = nullptr;
	std::string _name;
	bool _es = false;
};

}
