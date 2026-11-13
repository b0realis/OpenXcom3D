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
 * FirstPersonXrSmoothing.cpp: the eye pictures smoothed by the headset's graphics chip (see the
 * header). Each pass draws one big triangle over the target, every output pixel working out its
 * colour from the picture's pixels round it (texelFetch: no filtering by the chip):
 *
 *   FSR 1, after AMD's FidelityFX FSR 1.0 (MIT licence, (c) Advanced Micro Devices): EASU finds the
 *   way an edge runs from the luma round the pixel and weighs the 12 pixels nearest with a
 *   Lanczos-like window stretched along it, clamped to the 4 nearest (no ringing); RCAS then
 *   sharpens what EASU softened, limited so nothing over- or undershoots.
 *   SABR v3.0 (Joshua Street, from Hyllian's xBR; GPL 2 or later): the pixel art scaler: at each
 *   corner of a pixel the 21 round it decide whether an edge runs past at 45, 30 or 60 degrees,
 *   and the corner is blended with its neighbour along that line (smoothly, smoothstep).
 *
 * The pictures hold the game's sRGB colours as they are; the filters work on them as they are
 * (as the game's pixels look), and write linear light to an sRGB target, which the chip encodes
 * back (a pixel no filter changed comes out as it went in).
 */
#if defined(__ANDROID__) || defined(OPENXCOM3D_SMOOTHING_TOOL)
#include "FirstPersonXrSmoothing.h"
#include <algorithm>
#include <cmath>
#include <GLES3/gl3.h>

namespace OpenXcom
{

namespace FirstPerson
{

namespace Xr
{

namespace
{

/// RCAS's sharpening, in stops (0 the most; FSR's usual 0.2).
const float RcasSharpnessStops = 0.25f;

/// Every pass's vertex shader: one triangle over the whole target (no vertex buffer).
const char *const VertexShader = R"(#version 300 es
void main()
{
	vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// What every colour pass shares: the picture, its last pixel, the target's pixel in it, sRGB to linear light.
const char *const CommonHeader = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D uSource;   // the picture (its colours as they are), read pixel by pixel
uniform ivec2 uLast;         // its last pixel (width - 1, height - 1)
uniform vec2 uScale;         // its size over the target's
uniform bool uLinearize;     // the target is sRGB: write linear light
out vec4 outColour;
vec3 pixelAt(ivec2 p)
{
	return texelFetch(uSource, clamp(p, ivec2(0), uLast), 0).rgb;
}
vec3 toLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
)";

/// FSR 1's EASU (see the file's comment): into the middle picture, colours as they are.
const char *const EasuShader = R"(
float luma(vec3 c)
{
	return c.b * 0.5 + (c.r * 0.5 + c.g);
}
// The edge's way and how much of an edge it is, from the '+' round a pixel (a above, b left, c the pixel, d right, e
// below), weighted by its share of the bilinear position.
void addEdge(inout vec2 way, inout float edge, float w, float lA, float lB, float lC, float lD, float lE)
{
	float lenX = 1.0 / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0);
	float wayX = lD - lB;
	way.x += wayX * w;
	lenX = clamp(abs(wayX) * lenX, 0.0, 1.0);
	edge += lenX * lenX * w;
	float lenY = 1.0 / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0);
	float wayY = lE - lA;
	way.y += wayY * w;
	lenY = clamp(abs(wayY) * lenY, 0.0, 1.0);
	edge += lenY * lenY * w;
}
// One pixel's weight in the window turned along the edge, and its colour added.
void addTap(inout vec3 sum, inout float weights, vec2 offset, vec2 way, vec2 stretch, float lobe, float clip, vec3 colour)
{
	vec2 v = vec2(offset.x * way.x + offset.y * way.y, offset.x * -way.y + offset.y * way.x) * stretch;
	float d2 = min(dot(v, v), clip);
	float base = 2.0 / 5.0 * d2 - 1.0;
	float window = lobe * d2 - 1.0;
	base = 25.0 / 16.0 * base * base - (25.0 / 16.0 - 1.0);
	float w = base * window * window;
	sum += colour * w;
	weights += w;
}
void main()
{
	// The target pixel's place in the picture: the pixel f (at fp) and how far past it.
	vec2 pp = floor(gl_FragCoord.xy) * uScale + (0.5 * uScale - 0.5);
	vec2 fp = floor(pp);
	pp -= fp;
	ivec2 f = ivec2(fp);
	//    b c
	//  e f g h
	//  i j k l
	//    n o
	vec3 b = pixelAt(f + ivec2(0, -1)), c = pixelAt(f + ivec2(1, -1));
	vec3 e = pixelAt(f + ivec2(-1, 0)), ff = pixelAt(f), g = pixelAt(f + ivec2(1, 0)), h = pixelAt(f + ivec2(2, 0));
	vec3 i = pixelAt(f + ivec2(-1, 1)), j = pixelAt(f + ivec2(0, 1)), k = pixelAt(f + ivec2(1, 1)), l = pixelAt(f + ivec2(2, 1));
	vec3 n = pixelAt(f + ivec2(0, 2)), o = pixelAt(f + ivec2(1, 2));
	float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(ff), gL = luma(g), hL = luma(h);
	float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);
	// The edge's way and strength, from the four pixels round the place.
	vec2 way = vec2(0.0);
	float edge = 0.0;
	addEdge(way, edge, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	addEdge(way, edge, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
	addEdge(way, edge, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
	addEdge(way, edge, pp.x * pp.y, gL, jL, kL, lL, oL);
	float wayLength2 = dot(way, way);
	bool noEdge = wayLength2 < 1.0 / 32768.0;
	way = noEdge ? vec2(1.0, 0.0) : way * inversesqrt(wayLength2);
	edge = edge * 0.5;
	edge *= edge;
	// The window: stretched along the edge (up to sqrt(2) on a diagonal), narrower across it, its negative lobe less on an edge.
	float diagonal = dot(way, way) / max(abs(way.x), abs(way.y));
	vec2 stretch = vec2(1.0 + (diagonal - 1.0) * edge, 1.0 - 0.5 * edge);
	float lobe = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * edge;
	float clip = 1.0 / lobe;
	vec3 lowest = min(min(ff, g), min(j, k));
	vec3 highest = max(max(ff, g), max(j, k));
	vec3 sum = vec3(0.0);
	float weights = 0.0;
	addTap(sum, weights, vec2(0.0, -1.0) - pp, way, stretch, lobe, clip, b);
	addTap(sum, weights, vec2(1.0, -1.0) - pp, way, stretch, lobe, clip, c);
	addTap(sum, weights, vec2(-1.0, 1.0) - pp, way, stretch, lobe, clip, i);
	addTap(sum, weights, vec2(0.0, 1.0) - pp, way, stretch, lobe, clip, j);
	addTap(sum, weights, vec2(0.0, 0.0) - pp, way, stretch, lobe, clip, ff);
	addTap(sum, weights, vec2(-1.0, 0.0) - pp, way, stretch, lobe, clip, e);
	addTap(sum, weights, vec2(1.0, 1.0) - pp, way, stretch, lobe, clip, k);
	addTap(sum, weights, vec2(2.0, 1.0) - pp, way, stretch, lobe, clip, l);
	addTap(sum, weights, vec2(2.0, 0.0) - pp, way, stretch, lobe, clip, h);
	addTap(sum, weights, vec2(1.0, 0.0) - pp, way, stretch, lobe, clip, g);
	addTap(sum, weights, vec2(1.0, 2.0) - pp, way, stretch, lobe, clip, o);
	addTap(sum, weights, vec2(0.0, 2.0) - pp, way, stretch, lobe, clip, n);
	outColour = vec4(min(highest, max(lowest, sum / weights)), 1.0);
}
)";

/// FSR 1's RCAS (see the file's comment): the middle picture sharpened into the target, the same size.
const char *const RcasShader = R"(
uniform float uSharpness;    // exp2(-stops)
void main()
{
	//    b
	//  d e f
	//    h
	ivec2 p = ivec2(gl_FragCoord.xy);
	vec3 b = pixelAt(p + ivec2(0, -1)), d = pixelAt(p + ivec2(-1, 0)), e = pixelAt(p), f = pixelAt(p + ivec2(1, 0)), h = pixelAt(p + ivec2(0, 1));
	vec3 lowest = min(min(b, d), min(f, h));
	vec3 highest = max(max(b, d), max(f, h));
	// How far the ring may pull the pixel before it would go past black or white (per channel), the least of them.
	vec3 hitLow = min(lowest, e) / max(4.0 * highest, vec3(1.0 / 65536.0));
	vec3 hitHigh = (1.0 - max(highest, e)) / min(4.0 * lowest - 4.0, vec3(-1.0 / 65536.0));
	vec3 lobes = max(-hitLow, hitHigh);
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * uSharpness;
	vec3 colour = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
	outColour = vec4(uLinearize ? toLinear(colour) : colour, 1.0);
}
)";

/// SABR v3.0 (see the file's comment), into the target.
const char *const SabrShader = R"(
const vec4 Ai  = vec4( 1.0, -1.0, -1.0,  1.0);
const vec4 B45 = vec4( 1.0,  1.0, -1.0, -1.0);
const vec4 C45 = vec4( 1.5,  0.5, -0.5,  0.5);
const vec4 B30 = vec4( 0.5,  2.0, -0.5, -2.0);
const vec4 C30 = vec4( 1.0,  1.0, -0.5,  0.0);
const vec4 B60 = vec4( 2.0,  0.5, -2.0, -0.5);
const vec4 C60 = vec4( 2.0,  0.0, -1.0,  0.5);
const vec4 M45 = vec4(0.4, 0.4, 0.4, 0.4);
const vec4 M30 = vec4(0.2, 0.4, 0.2, 0.4);
const vec4 M60 = vec4(0.4, 0.2, 0.4, 0.2);
const vec4 Mshift = vec4(0.2);
const float coef = 2.0;
const vec4 threshold = vec4(0.32);
const vec3 lum = vec3(0.21, 0.72, 0.07);
vec4 lumTo(vec3 v0, vec3 v1, vec3 v2, vec3 v3) { return vec4(dot(lum, v0), dot(lum, v1), dot(lum, v2), dot(lum, v3)); }
vec4 lumDf(vec4 a, vec4 b) { return abs(a - b); }
bvec4 lumEq(vec4 a, vec4 b) { return lessThan(lumDf(a, b), threshold); }
vec4 lumWd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) { return lumDf(a, b) + lumDf(a, c) + lumDf(d, e) + lumDf(d, f) + 4.0 * lumDf(g, h); }
bvec4 and4(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
bvec4 or4(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
float cDf(vec3 c1, vec3 c2) { vec3 d = abs(c1 - c2); return d.r + d.g + d.b; }
void main()
{
	// The target pixel's place in the picture: its pixel (P12) and where in it.
	vec2 place = (floor(gl_FragCoord.xy) + 0.5) * uScale;
	ivec2 c = ivec2(floor(place));
	vec2 fp = fract(place);
	//      1  2  3
	//   5  6  7  8  9
	//  10 11 12 13 14
	//  15 16 17 18 19
	//     21 22 23
	vec3 P1 = pixelAt(c + ivec2(-1, -2)), P2 = pixelAt(c + ivec2(0, -2)), P3 = pixelAt(c + ivec2(1, -2));
	vec3 P5 = pixelAt(c + ivec2(-2, -1)), P6 = pixelAt(c + ivec2(-1, -1)), P7 = pixelAt(c + ivec2(0, -1)), P8 = pixelAt(c + ivec2(1, -1)), P9 = pixelAt(c + ivec2(2, -1));
	vec3 P10 = pixelAt(c + ivec2(-2, 0)), P11 = pixelAt(c + ivec2(-1, 0)), P12 = pixelAt(c), P13 = pixelAt(c + ivec2(1, 0)), P14 = pixelAt(c + ivec2(2, 0));
	vec3 P15 = pixelAt(c + ivec2(-2, 1)), P16 = pixelAt(c + ivec2(-1, 1)), P17 = pixelAt(c + ivec2(0, 1)), P18 = pixelAt(c + ivec2(1, 1)), P19 = pixelAt(c + ivec2(2, 1));
	vec3 P21 = pixelAt(c + ivec2(-1, 2)), P22 = pixelAt(c + ivec2(0, 2)), P23 = pixelAt(c + ivec2(1, 2));
	vec4 p7 = lumTo(P7, P11, P17, P13);
	vec4 p8 = lumTo(P8, P6, P16, P18);
	vec4 p11 = p7.yzwx;
	vec4 p12 = lumTo(P12, P12, P12, P12);
	vec4 p13 = p7.wxyz;
	vec4 p14 = lumTo(P14, P2, P10, P22);
	vec4 p16 = p8.zwxy;
	vec4 p17 = p7.zwxy;
	vec4 p18 = p8.wxyz;
	vec4 p19 = lumTo(P19, P3, P5, P21);
	vec4 p22 = p14.wxyz;
	vec4 p23 = lumTo(P23, P9, P1, P15);
	// How much each corner may blend, along a 45, 30 or 60 degree line.
	vec4 ma45 = smoothstep(C45 - M45, C45 + M45, Ai * fp.y + B45 * fp.x);
	vec4 ma30 = smoothstep(C30 - M30, C30 + M30, Ai * fp.y + B30 * fp.x);
	vec4 ma60 = smoothstep(C60 - M60, C60 + M60, Ai * fp.y + B60 * fp.x);
	vec4 marn = smoothstep(C45 - M45 + Mshift, C45 + M45 + Mshift, Ai * fp.y + B45 * fp.x);
	// Which way the edges run at each corner.
	vec4 e45 = lumWd(p12, p8, p16, p18, p22, p14, p17, p13);
	vec4 econt = lumWd(p17, p11, p23, p13, p7, p19, p12, p18);
	vec4 e30 = lumDf(p13, p16);
	vec4 e60 = lumDf(p8, p17);
	bvec4 r45_1 = and4(notEqual(p12, p13), notEqual(p12, p17));
	bvec4 r45_2 = and4(not(lumEq(p13, p7)), not(lumEq(p13, p8)));
	bvec4 r45_3 = and4(not(lumEq(p17, p11)), not(lumEq(p17, p16)));
	bvec4 r45_4_1 = and4(not(lumEq(p13, p14)), not(lumEq(p13, p19)));
	bvec4 r45_4_2 = and4(not(lumEq(p17, p22)), not(lumEq(p17, p23)));
	bvec4 r45_4 = and4(lumEq(p12, p18), or4(r45_4_1, r45_4_2));
	bvec4 r45_5 = or4(lumEq(p12, p16), lumEq(p12, p8));
	bvec4 r45 = and4(r45_1, or4(or4(or4(r45_2, r45_3), r45_4), r45_5));
	bvec4 r30 = and4(notEqual(p12, p16), notEqual(p11, p16));
	bvec4 r60 = and4(notEqual(p12, p8), notEqual(p7, p8));
	bvec4 edr45 = and4(lessThan(e45, econt), r45);
	bvec4 edrrn = lessThanEqual(e45, econt);
	bvec4 edr30 = and4(lessThanEqual(coef * e30, e60), r30);
	bvec4 edr60 = and4(lessThanEqual(coef * e60, e30), r60);
	vec4 final45 = vec4(and4(and4(not(edr30), not(edr60)), edr45));
	vec4 final30 = vec4(and4(and4(edr45, not(edr60)), edr30));
	vec4 final60 = vec4(and4(and4(edr45, not(edr30)), edr60));
	vec4 final36 = vec4(and4(and4(edr60, edr30), edr45));
	vec4 finalrn = vec4(and4(not(edr45), edrrn));
	vec4 px = step(lumDf(p12, p17), lumDf(p12, p13));
	vec4 mac = final36 * max(ma30, ma60) + final30 * ma30 + final60 * ma60 + final45 * ma45 + finalrn * marn;
	// The corners blended round the pixel both ways; the one further from the pixel's own colour wins.
	vec3 res1 = P12;
	res1 = mix(res1, mix(P13, P17, px.x), mac.x);
	res1 = mix(res1, mix(P7, P13, px.y), mac.y);
	res1 = mix(res1, mix(P11, P7, px.z), mac.z);
	res1 = mix(res1, mix(P17, P11, px.w), mac.w);
	vec3 res2 = P12;
	res2 = mix(res2, mix(P17, P11, px.w), mac.w);
	res2 = mix(res2, mix(P11, P7, px.z), mac.z);
	res2 = mix(res2, mix(P7, P13, px.y), mac.y);
	res2 = mix(res2, mix(P13, P17, px.x), mac.x);
	vec3 colour = mix(res1, res2, step(cDf(P12, res1), cDf(P12, res2)));
	outColour = vec4(uLinearize ? toLinear(colour) : colour, 1.0);
}
)";

/// The depth pass: each depth value over its block of the target (nearest).
const char *const DepthShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D uSource;   // the depth (R32F), read pixel by pixel
uniform ivec2 uLast;
uniform vec2 uScale;
void main()
{
	ivec2 p = ivec2(floor((floor(gl_FragCoord.xy) + 0.5) * uScale));
	gl_FragDepth = texelFetch(uSource, clamp(p, ivec2(0), uLast), 0).r;
}
)";

/**
 * Compiles and links a program; false (and the driver's words in error) if it fails.
 * @param fragment The fragment shader's source (or its parts, joined).
 * @param program Set to the program.
 * @param error Set to why it failed.
 */
bool makeProgram(const std::string &fragment, unsigned int &program, std::string &error)
{
	auto compile = [&error](GLenum type, const std::string &source) -> GLuint
	{
		GLuint shader = glCreateShader(type);
		const char *text = source.c_str();
		glShaderSource(shader, 1, &text, nullptr);
		glCompileShader(shader);
		GLint compiled = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			char log[1024] = "";
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			error = std::string(type == GL_VERTEX_SHADER ? "vertex shader: " : "fragment shader: ") + log;
			glDeleteShader(shader);
			return 0;
		}
		return shader;
	};
	GLuint vertex = compile(GL_VERTEX_SHADER, VertexShader);
	GLuint pixels = vertex ? compile(GL_FRAGMENT_SHADER, fragment) : 0;
	if (!vertex || !pixels)
	{
		if (vertex) glDeleteShader(vertex);
		return false;
	}
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, pixels);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(pixels);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char log[1024] = "";
		glGetProgramInfoLog(program, sizeof(log), nullptr, log);
		error = std::string("linking: ") + log;
		glDeleteProgram(program);
		program = 0;
		return false;
	}
	return true;
}

/**
 * Makes (or remakes at a new size) a texture read pixel by pixel.
 * @param texture The texture (0: made now).
 * @param width Its width now (set to the new one).
 * @param height Its height now (set to the new one).
 * @param newWidth The width wanted.
 * @param newHeight The height wanted.
 * @param internalFormat Its format (GL_RGBA8, GL_R32F).
 */
void makeTexture(unsigned int &texture, int &width, int &height, int newWidth, int newHeight, GLenum internalFormat)
{
	if (texture && width == newWidth && height == newHeight)
	{
		return;
	}
	if (texture)
	{
		glDeleteTextures(1, &texture);
	}
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, newWidth, newHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	width = newWidth;
	height = newHeight;
}

/**
 * Sets a colour pass's uniforms: the picture's texture unit, its last pixel, its size over the target's, sRGB.
 */
void setPassUniforms(unsigned int program, int width, int height, int targetWidth, int targetHeight, bool linearize)
{
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "uSource"), 0);
	glUniform2i(glGetUniformLocation(program, "uLast"), width - 1, height - 1);
	glUniform2f(glGetUniformLocation(program, "uScale"), (float)width / targetWidth, (float)height / targetHeight);
	const GLint linearizeAt = glGetUniformLocation(program, "uLinearize");
	if (linearizeAt >= 0) glUniform1i(linearizeAt, linearize ? 1 : 0);
}

/**
 * Draws one pass into a texture: the framebuffer on it (as colour, or as depth), the whole of it covered.
 * @return False when the framebuffer isn't complete.
 */
bool drawPass(unsigned int framebuffer, unsigned int vertexArray, unsigned int target, int targetWidth, int targetHeight, bool depth)
{
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, depth ? 0 : target, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth ? target : 0, 0);
	const GLenum colourBuffer = depth ? GL_NONE : GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &colourBuffer);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}
	glViewport(0, 0, targetWidth, targetHeight);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	if (depth)
	{
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_TRUE);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}
	glBindVertexArray(vertexArray);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

}

/**
 * The scale for an eye picture: SmoothingScale, less for one whose bigger side would pass SmoothingMaxSize (at least 1).
 * @param width The picture's width.
 * @param height Its height.
 */
int smoothingScaleFor(int width, int height)
{
	const int side = std::max(1, std::max(width, height));
	return std::max(1, std::min(SmoothingScale, SmoothingMaxSize / side));
}

/**
 * Makes the filter's programs (and the depth pass's), a framebuffer and a vertex array, on the current context.
 * @param filter The filter.
 * @param error Set to why it failed.
 * @return True when made.
 */
bool EyeSmoother::create(SmoothingFilter filter, std::string &error)
{
	destroy();
	_filter = filter;
	const std::string common = CommonHeader;
	bool made = filter == SmoothingFilter::Fsr ? makeProgram(common + EasuShader, _firstPass, error) && makeProgram(common + RcasShader, _secondPass, error)
		: makeProgram(common + SabrShader, _firstPass, error);
	made = made && makeProgram(DepthShader, _depthPass, error);
	if (!made)
	{
		destroy();
		return false;
	}
	glGenFramebuffers(1, &_framebuffer);
	glGenVertexArrays(1, &_vertexArray);
	_ready = true;
	return true;
}

/// Deletes the programs, textures, framebuffer and vertex array.
void EyeSmoother::destroy()
{
	for (unsigned int *program : { &_firstPass, &_secondPass, &_depthPass })
	{
		if (*program) glDeleteProgram(*program);
		*program = 0;
	}
	for (unsigned int *texture : { &_source, &_middle, &_depthSource })
	{
		if (*texture) glDeleteTextures(1, texture);
		*texture = 0;
	}
	if (_framebuffer) glDeleteFramebuffers(1, &_framebuffer);
	if (_vertexArray) glDeleteVertexArrays(1, &_vertexArray);
	_framebuffer = _vertexArray = 0;
	_sourceWidth = _sourceHeight = _middleWidth = _middleHeight = _depthWidth = _depthHeight = 0;
	_ready = false;
}

/**
 * Draws a picture bigger into a colour texture: uploaded, then FSR's two passes (EASU into the middle picture, RCAS into
 * the target) or SABR's one.
 * @param pixels The picture (RGBA bytes, the bottom row first).
 * @param width Its width.
 * @param height Its height.
 * @param target The colour texture.
 * @param targetWidth Its width.
 * @param targetHeight Its height.
 * @param srgbTarget The texture is sRGB.
 * @return False on an error (the framebuffer, or OpenGL's).
 */
bool EyeSmoother::drawColour(const uint32_t *pixels, int width, int height, unsigned int target, int targetWidth, int targetHeight, bool srgbTarget)
{
	if (!_ready || width <= 0 || height <= 0)
	{
		return false;
	}
	makeTexture(_source, _sourceWidth, _sourceHeight, width, height, GL_RGBA8);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _source);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	return drawColourFrom(_source, width, height, target, targetWidth, targetHeight, srgbTarget);
}

/**
 * Draws a picture that is a texture already bigger into a colour texture: FSR's two passes or SABR's one (see drawColour).
 * @param source The picture's texture (RGBA8, the bottom row first).
 */
bool EyeSmoother::drawColourFrom(unsigned int source, int width, int height, unsigned int target, int targetWidth, int targetHeight, bool srgbTarget)
{
	if (!_ready || width <= 0 || height <= 0 || !source)
	{
		return false;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source);
	bool drawn;
	if (_filter == SmoothingFilter::Fsr)
	{
		makeTexture(_middle, _middleWidth, _middleHeight, targetWidth, targetHeight, GL_RGBA8);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, source);
		setPassUniforms(_firstPass, width, height, targetWidth, targetHeight, false);
		drawn = drawPass(_framebuffer, _vertexArray, _middle, targetWidth, targetHeight, false);
		glBindTexture(GL_TEXTURE_2D, _middle);
		setPassUniforms(_secondPass, targetWidth, targetHeight, targetWidth, targetHeight, srgbTarget);
		glUniform1f(glGetUniformLocation(_secondPass, "uSharpness"), std::exp2(-RcasSharpnessStops));
		drawn = drawn && drawPass(_framebuffer, _vertexArray, target, targetWidth, targetHeight, false);
	}
	else
	{
		setPassUniforms(_firstPass, width, height, targetWidth, targetHeight, srgbTarget);
		drawn = drawPass(_framebuffer, _vertexArray, target, targetWidth, targetHeight, false);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return drawn && glGetError() == GL_NO_ERROR;
}

/**
 * Draws a depth picture bigger into a depth texture, each value over its block.
 * @param depth The depth (0 to 1, the bottom row first).
 * @param width Its width.
 * @param height Its height.
 * @param target The depth texture.
 * @param targetWidth Its width.
 * @param targetHeight Its height.
 * @return False on an error.
 */
bool EyeSmoother::drawDepth(const float *depth, int width, int height, unsigned int target, int targetWidth, int targetHeight)
{
	if (!_ready || width <= 0 || height <= 0)
	{
		return false;
	}
	makeTexture(_depthSource, _depthWidth, _depthHeight, width, height, GL_R32F);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _depthSource);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_FLOAT, depth);
	return drawDepthFrom(_depthSource, width, height, target, targetWidth, targetHeight);
}

/**
 * Draws a depth picture that is a texture already bigger into a depth texture (see drawDepth).
 * @param source The depth's texture (R32F, 0 to 1, the bottom row first).
 */
bool EyeSmoother::drawDepthFrom(unsigned int source, int width, int height, unsigned int target, int targetWidth, int targetHeight)
{
	if (!_ready || width <= 0 || height <= 0 || !source)
	{
		return false;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source);
	setPassUniforms(_depthPass, width, height, targetWidth, targetHeight, false);
	const bool drawn = drawPass(_framebuffer, _vertexArray, target, targetWidth, targetHeight, true);
	glDisable(GL_DEPTH_TEST);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return drawn && glGetError() == GL_NO_ERROR;
}

}

}

}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
