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
 * The 3D view's units and effects on the graphics card (setting "Renderer": GPU).
 *
 * After the rays the view draws what is in the world over their picture: marks, items, units,
 * fire, shots, smoke. On the CPU each of those passes reads and writes the view's buffers. With
 * the rays on the card that meant reading their pictures back first, and at a high resolution the
 * passes cost as much as the rays (smoke and the units most).
 *
 * Here the same passes run, but while the view records (_gpuRecording) their funnels (drawSprite,
 * drawGroundShade, drawLine3D, the marks' and the dots' loops) write down what they would draw
 * instead of drawing it: a primitive each, in the order drawn. The card then draws them over the
 * rays' pictures, which never left it (FirstPersonGpu::drawEffects, EffectsSource), and the
 * finished pictures come back once.
 *
 * A primitive is FirstPersonGpu::Effects::Texels texels of four floats; texel 0 is its kind, its
 * flags and two numbers of its own, texel 1 its box on the screen (left, top, and one past its
 * right and bottom), the rest as each record... function below lays them out. The camera's
 * sums are done here in double precision, as the CPU's routines do them; the card works a pixel
 * out in single precision, so a sprite's edge may fall a pixel off the CPU's now and then.
 *
 * OPENXCOM3D_GPU_EFFECTS=0: the old way (the rays read back, the passes on the CPU), to compare; =check: both, the CPU's
 * shown, the card's compared with it a pixel at a time (see gpuCheckEffects).
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "FirstPersonGpu.h"
#include "FirstPersonSpriteHits.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * The sprites' bytes on the card (their pixels, and a see-through one's opacity map after them),
 * shared by every view and kept from frame to frame: a sprite is sent up once, found again by
 * what it is (a hash of its size and bytes; a frame composed for one unit is filled again for
 * the next, so where it is says nothing). Past SpriteAtlasMost bytes they are begun anew.
 */
struct SpriteAtlas
{
	std::vector<Uint8> bytes;
	std::unordered_map<Uint64, int> places; // where each sprite's bytes start, by their hash
	Uint32 generation = 0;
};
static const size_t SpriteAtlasMost = 12u << 20;
static SpriteAtlas &spriteAtlas()
{
	static SpriteAtlas atlas;
	return atlas;
}

/**
 * Where a picture's bytes are in the atlas, added to it if new (see SpriteAtlas).
 * @param pixels Its rows, pitch bytes from one to the next, width by height.
 * @param opacityMap A see-through sprite's opacity map (width x height bytes, kept right after its pixels), or null.
 */
static int atlasPlace(const Uint8 *pixels, int pitch, int width, int height, const Uint8 *opacityMap)
{
	SpriteAtlas &atlas = spriteAtlas();
	const size_t count = (size_t)width * height;
	Uint64 hash = 1469598103934665603ull ^ ((Uint64)width << 32 | (Uint32)height) ^ (opacityMap ? 0x9E3779B97F4A7C15ull : 0ull);
	auto mix = [&hash](const Uint8 *bytes, size_t size) {
		size_t at = 0;
		for (; at + 8 <= size; at += 8)
		{
			Uint64 eight;
			std::memcpy(&eight, bytes + at, 8);
			hash = (hash ^ eight) * 1099511628211ull;
			hash ^= hash >> 29;
		}
		for (; at < size; ++at) hash = (hash ^ bytes[at]) * 1099511628211ull;
	};
	for (int row = 0; row < height; ++row) mix(pixels + (size_t)row * pitch, width);
	if (opacityMap) mix(opacityMap, count);
	const auto known = atlas.places.find(hash);
	if (known != atlas.places.end())
	{
		return known->second;
	}
	const int at = (int)atlas.bytes.size();
	atlas.bytes.resize(at + count);
	for (int row = 0; row < height; ++row)
	{
		std::memcpy(&atlas.bytes[at + (size_t)row * width], pixels + (size_t)row * pitch, width);
	}
	if (opacityMap) atlas.bytes.insert(atlas.bytes.end(), opacityMap, opacityMap + count);
	atlas.places.emplace(hash, at);
	return at;
}

/**
 * A frame's units and effects as recorded for the card, and what is needed to read its answer:
 * each primitive's box on the screen (for the bins), where each sprite's bytes went, and whose
 * picture each sprite is (the card says which sprite shows round the pointer, by number).
 */
struct GpuEffectList
{
	FirstPersonGpu::Effects effects;
	std::vector<std::array<int, 4>> boxes;                 // each primitive's box: left, top, one past its right, one past its bottom
	std::unordered_map<const void*, int> spritesByAddress; // this frame's see-through sprites, by where their opacity maps are: a cloud has hundreds of puffs of a few pictures, each looked up in the atlas once
	struct Pictured { BattleUnit *unit, *body; const Tile *pile; };
	std::vector<Pictured> pictured;                        // per primitive: whose picture it is
	std::vector<Uint32> notes;                             // the card's notes, four numbers a pixel
	std::vector<Uint32> patchNotes;                        // ... or only those round the pointer
	std::vector<int> binCounts;
	std::vector<float> outlines;                           // the outlined pictures' records, if the frame has outlines (see gpuRecordOutlines)

	void clear()
	{
		effects.prims.clear();
		effects.outlineAt = -1;
		effects.outlineCount = 0;
		outlines.clear();
		boxes.clear();
		spritesByAddress.clear();
		SpriteAtlas &atlas = spriteAtlas();
		if (atlas.bytes.size() > SpriteAtlasMost)
		{
			atlas.bytes.clear();
			atlas.places.clear();
			++atlas.generation;
		}
		effects.bytes = &atlas.bytes;
		effects.bytesGeneration = atlas.generation;
		pictured.clear();
	}
	/// A new primitive of a kind: its texels (all 0 but the kind and the box), and whose picture it is.
	float *add(int kind, int left, int top, int right, int bottom)
	{
		const size_t at = effects.prims.size();
		effects.prims.resize(at + FirstPersonGpu::Effects::Texels * 4, 0.0f);
		float *texels = &effects.prims[at];
		texels[0] = (float)kind;
		texels[4] = (float)left; texels[5] = (float)top; texels[6] = (float)right; texels[7] = (float)bottom;
		boxes.push_back({ left, top, right, bottom });
		pictured.push_back({ nullptr, nullptr, nullptr });
		return texels;
	}
};

/**
 * Whether this frame's units and effects go to the card, with its rays.
 * @return 0 no; 1 yes; 2 yes, beside the CPU's own, which is shown, the two compared (OPENXCOM3D_GPU_EFFECTS=check).
 */
int FirstPersonView::gpuEffectsWanted() const
{
	static const char *const asked = getenv("OPENXCOM3D_GPU_EFFECTS");
	const int mode = !asked ? 1 : !strcmp(asked, "check") ? 2 : atoi(asked) != 0 ? 1 : 0;
	return mode;
}

/**
 * Starts recording the frame's units and effects (the rays' pictures are kept on the card).
 */
void FirstPersonView::gpuBeginEffects()
{
	if (!_gpuEffects) _gpuEffects = std::make_shared<GpuEffectList>();
	_gpuEffects->clear();
	_gpuRecording = true;
}

/**
 * Records a sprite standing in the world: the card as drawSprite sets it up (keep the two alike),
 * in the camera's frame, and the sprite's bytes.
 * Texel 0: kind, flags (1 lying, 2 a unit's, 4 keeps its own colours, 8 a pile's, 16 see-through), shade, depth bias.
 * Texel 2: the anchor from the eye, and how far across it is; 3: the way up the card, and how far the anchor is along
 * the way the card faces; 4: the way the card faces, and how far ahead the anchor is along the ground.
 * Texel 5: a sprite pixel's size across, up the card and along the floor, and the floor's height; 6: the floor's slope,
 * the anchor's rise on the image plane, the anchor's column and row.
 * Texel 7: the sprite's width and height, where its bytes are, where its opacity map's are (-1: none).
 * Texel 8: the height (from the eye) nothing of the card shows below, the opacity up close, the see-through opacity, the smoke's cap.
 */
void FirstPersonView::gpuRecordSprite(const SpriteDraw &draw)
{
	const Surface *sprite = draw.sprite;
	const double *world = draw.world;
	double anchorScreenX, anchorScreenY, viewDepth;
	if (!sprite || !projectToScreen(world, anchorScreenX, anchorScreenY, viewDepth))
	{
		return;
	}
	const SeeThroughSprite *seeThroughSprite = draw.seeThrough;
	const float opacity = (float)std::clamp((viewDepth - 4.0) / 6.0, 0.0, 1.0);
	const float seeThroughOpacity = seeThroughSprite ? seeThroughSprite->opacity * opacity : 1.0f;
	if (opacity <= 0.0f || seeThroughOpacity * SeeThroughLevels < 0.5f)
	{
		return;
	}
	const int width = getWidth(), height = getHeight();
	const int spriteWidth = sprite->getWidth(), spriteHeight = sprite->getHeight();
	const double focalLength = (width / 2.0) / _halfFovTangent;
	auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	const double level[2] = { std::cos(_yaw), std::sin(_yaw) };
	const double anchor[3] = { world[0] - _eye[0], world[1] - _eye[1], (world[2] - _eye[2]) * ZScale };
	const double elevation = std::atan2(anchor[2], std::sqrt(anchor[0] * anchor[0] + anchor[1] * anchor[1]));
	double tilt = std::clamp(_pitch, -CardTiltMax * Pi / 180.0, CardTiltUpMax * Pi / 180.0);
	if (elevation > CardTiltUpMax * Pi / 180.0)
	{
		tilt = std::max(tilt, std::min(elevation - CardTiltUpMax * Pi / 180.0, CardTiltSteepMax * Pi / 180.0));
	}
	const double cardUp[3] = { -std::sin(tilt) * level[0], -std::sin(tilt) * level[1], std::cos(tilt) };
	const double cardFacing[3] = { std::cos(tilt) * level[0], std::cos(tilt) * level[1], std::sin(tilt) };
	const double anchorAcross = dot(anchor, _levelRight), anchorFacing = dot(anchor, cardFacing);
	const double columnSize = draw.scale * SpritePixelX, rowSize = draw.scale * ZScale * draw.standingSquash, lyingRowSize = draw.scale * LyingRowVoxels;
	const double floorHeight = anchor[2] + 0.5 * ZScale;
	const double floorSlope = draw.lyingSlope * ZScale;
	const double anchorAlong = anchor[0] * level[0] + anchor[1] * level[1];
	const bool lying = draw.pose == SpritePose::Lying;
	const int anchorX = draw.anchorX, anchorY = draw.anchorY;

	// Its box on the screen: round the card's corners and, lying, the floor's in front of the anchor
	// (all of the view if one is behind the camera), a pixel more each way.
	int left = width, right = 0, top = height, bottom = 0;
	auto addCorners = [&](const double middle[3]) {
		for (const double across : { -anchorX * columnSize, (spriteWidth - anchorX) * columnSize })
		{
			const double corner[3] = { middle[0] + _levelRight[0] * across, middle[1] + _levelRight[1] * across, middle[2] + _levelRight[2] * across };
			const double depth = dot(corner, _forward);
			if (depth <= 1.0)
			{
				left = top = 0; right = width; bottom = height;
				continue;
			}
			const double screenX = width / 2.0 + dot(corner, _right) * focalLength / depth, screenY = height / 2.0 - dot(corner, _up) * focalLength / depth;
			left = std::min(left, (int)std::floor(screenX)); right = std::max(right, (int)std::ceil(screenX));
			top = std::min(top, (int)std::floor(screenY)); bottom = std::max(bottom, (int)std::ceil(screenY));
		}
	};
	double topMiddle[3], bottomMiddle[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		topMiddle[axis] = anchor[axis] + cardUp[axis] * anchorY * rowSize;
		bottomMiddle[axis] = anchor[axis] - cardUp[axis] * (spriteHeight - anchorY) * rowSize;
	}
	addCorners(topMiddle);
	if (lying)
	{
		const double reach = (spriteHeight - anchorY) * lyingRowSize;
		const double farFloor[3] = { anchor[0], anchor[1], floorHeight };
		const double nearFloor[3] = { anchor[0] - level[0] * reach, anchor[1] - level[1] * reach, floorHeight + floorSlope * reach };
		addCorners(anchor);
		addCorners(farFloor);
		addCorners(nearFloor);
	}
	else
	{
		addCorners(bottomMiddle);
	}
	left = std::max(0, left - 1); top = std::max(0, top - 1); right = std::min(width, right + 1); bottom = std::min(height, bottom + 1);
	if (left >= right || top >= bottom)
	{
		return;
	}

	// Its bytes, in the atlas (see SpriteAtlas): found by their hash, sent up if new.
	GpuEffectList &list = *_gpuEffects;
	const SDL_Surface *surface = const_cast<Surface*>(sprite)->getSurface(); // read only
	const size_t spriteBytes = (size_t)spriteWidth * spriteHeight;
	const Uint8 *opacityMap = seeThroughSprite ? seeThroughSprite->opacityMap : nullptr;
	auto placeInAtlas = [&]() { return atlasPlace((const Uint8 *)surface->pixels, surface->pitch, spriteWidth, spriteHeight, opacityMap); };
	int pixelsAt;
	if (opacityMap)
	{
		auto known = list.spritesByAddress.find(opacityMap);
		if (known == list.spritesByAddress.end())
		{
			known = list.spritesByAddress.emplace(opacityMap, placeInAtlas()).first;
		}
		pixelsAt = known->second;
	}
	else
	{
		pixelsAt = placeInAtlas();
	}
	const int opacityAt = opacityMap ? pixelsAt + (int)spriteBytes : -1;

	float *texels = list.add(FirstPersonGpu::Effects::Sprite, left, top, right, bottom);
	list.pictured.back() = { draw.unit, draw.body, draw.pile };
	texels[1] = (float)((lying ? 1 : 0) | (draw.unit ? 2 : 0) | (draw.unit || draw.body || draw.pile ? 4 : 0) | (draw.pile ? 8 : 0) | (seeThroughSprite ? 16 : 0));
	texels[2] = (float)draw.shade;
	texels[3] = (float)draw.depthBias;
	const double numbers[28] = {
		anchor[0], anchor[1], anchor[2], anchorAcross,
		cardUp[0], cardUp[1], cardUp[2], anchorFacing,
		cardFacing[0], cardFacing[1], cardFacing[2], anchorAlong,
		columnSize, rowSize, lyingRowSize, floorHeight,
		floorSlope, dot(anchor, _levelUp) / dot(anchor, _forward), (double)anchorX, (double)anchorY,
		(double)spriteWidth, (double)spriteHeight, (double)pixelsAt, (double)opacityAt,
		std::max(-1e9, draw.floorCut - _eye[2]), opacity, seeThroughOpacity, (double)(seeThroughSprite ? seeThroughSprite->capLevels : 0) };
	for (int which = 0; which < 28; ++which) texels[8 + which] = (float)numbers[which];
}

/**
 * Records a shade on the ground (drawGroundShade: a unit's, a scorch): its box as there.
 * Texel 0: kind, flags (1 stretched away from a light), the shades, the density.
 * Texel 2: its centre from the eye (across the map), the ground's height from the eye in true proportions, its radius.
 * Texel 3: the way it stretches, and how long.
 */
void FirstPersonView::gpuRecordGroundShade(const double centre[3], double radius, int steps, float density, const double *away, double length)
{
	const bool stretched = away && length > 0.0;
	const double reach = stretched ? radius * FlareShadowWidening : radius;
	const double endX = stretched ? centre[0] + away[0] * length : centre[0], endY = stretched ? centre[1] + away[1] * length : centre[1];
	const int width = getWidth(), height = getHeight();
	int left = width, top = height, right = -1, bottom = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double cornerPoint[3] = { (corner & 1) ? std::max(centre[0], endX) + reach : std::min(centre[0], endX) - reach,
			(corner & 2) ? std::max(centre[1], endY) + reach : std::min(centre[1], endY) - reach, centre[2] + 0.5 };
		double screenX, screenY, cornerDepth;
		if (!projectToScreen(cornerPoint, screenX, screenY, cornerDepth))
		{
			return;
		}
		left = std::min(left, (int)screenX); right = std::max(right, (int)screenX + 1);
		top = std::min(top, (int)screenY); bottom = std::max(bottom, (int)screenY + 1);
	}
	left = std::max(0, left); top = std::max(0, top); right = std::min(width - 1, right); bottom = std::min(height - 1, bottom);
	if (left > right || top > bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::GroundShade, left, top, right + 1, bottom + 1);
	texels[1] = stretched ? 1.0f : 0.0f;
	texels[2] = (float)steps;
	texels[3] = density;
	texels[8] = (float)(centre[0] - _eye[0]);
	texels[9] = (float)(centre[1] - _eye[1]);
	texels[10] = (float)((centre[2] + 0.5 - _eye[2]) * ZScale);
	texels[11] = (float)radius;
	texels[12] = stretched ? (float)away[0] : 0.0f;
	texels[13] = stretched ? (float)away[1] : 0.0f;
	texels[14] = stretched ? (float)length : 1.0f;
}

/**
 * Records a bullet hole or a scorch mark (drawDecals).
 * Texel 0: kind, flags (1 a ragged edge), its core's shades, its edge's.
 * Texel 2: where it falls on the screen, its depth, its radius; 3: its place right and up of the view axis, a
 * pixel's span there, the radius drawn.
 */
void FirstPersonView::gpuRecordDecal(double screenX, double screenY, double depth, double markRight, double markUp, double radius, double pixelSpan,
	double drawnRadius, int reach, int coreShades, int edgeShades)
{
	const int centreX = (int)std::floor(screenX), centreY = (int)std::floor(screenY);
	const int left = std::max(0, centreX - reach), top = std::max(0, centreY - reach), right = std::min(getWidth(), centreX + reach + 1), bottom = std::min(getHeight(), centreY + reach + 1);
	if (left >= right || top >= bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::Decal, left, top, right, bottom);
	texels[1] = reach > 1 ? 1.0f : 0.0f;
	texels[2] = (float)coreShades;
	texels[3] = (float)edgeShades;
	texels[8] = (float)centreX; texels[9] = (float)centreY; texels[10] = (float)depth; texels[11] = (float)radius;
	texels[12] = (float)markRight; texels[13] = (float)markUp; texels[14] = (float)pixelSpan; texels[15] = (float)drawnRadius;
}

/**
 * Records a line in the world (drawLine3D, which clips it and takes its ends to the screen first).
 * Texel 0: kind, flags (1 it glows, 2 over everything, 4 it leaves the glow as it is, 8 no voxel to spare in front of the scene), its colour, its thickness.
 * Texel 2: its ends on the screen; 3: one over each end's depth, its steps, its opacity; 4: its width in the world (0: its thickness).
 */
void FirstPersonView::gpuRecordLine(double startX, double startY, double startZ, double endX, double endY, double endZ, int steps, Uint8 color, int thickness, float opacity,
	float worldWidth, bool glows, bool overEverything, bool keepsGlow, bool exactDepth)
{
	const int left = std::max(0, (int)std::floor(std::min(startX, endX)) - thickness - 1), top = std::max(0, (int)std::floor(std::min(startY, endY)) - thickness - 1),
		right = std::min(getWidth(), (int)std::ceil(std::max(startX, endX)) + thickness + 2), bottom = std::min(getHeight(), (int)std::ceil(std::max(startY, endY)) + thickness + 2);
	if (left >= right || top >= bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::Line, left, top, right, bottom);
	texels[1] = (float)((glows ? 1 : 0) | (overEverything ? 2 : 0) | (keepsGlow ? 4 : 0) | (exactDepth ? 8 : 0));
	texels[2] = (float)color;
	texels[3] = (float)thickness;
	texels[8] = (float)startX; texels[9] = (float)startY; texels[10] = (float)endX; texels[11] = (float)endY;
	texels[12] = (float)(1.0 / startZ); texels[13] = (float)(1.0 / endZ); texels[14] = (float)steps; texels[15] = opacity;
	texels[16] = worldWidth;
}

/**
 * Records a square of pixels in front of the scene (an edge dot, a particle, a dot of the aimed path).
 * Texel 0: kind, flags (1 it glows, 2 it stops the glow, 4 its corners rounded off, 8 hidden only by what is nearer
 * than its leeway, not as near), its colour, its depth. Texel 2: its leeway in front of the scene, in voxels; its
 * other colour and the share of its pixels (by the dither) that are its first (below 0: all); how far it has faded
 * into the haze (the dither's pixels below that are left out).
 */
void FirstPersonView::gpuRecordDot(int left, int top, int dotWidth, int dotHeight, double depth, Uint8 color, int flags, float leeway, Uint8 otherColor, float firstShare, float haze)
{
	const int right = std::min(getWidth(), left + dotWidth), bottom = std::min(getHeight(), top + dotHeight);
	// (The box is the dot's own: the card finds its corners by it. One cut by the view's edge has no rounded corner there.)
	if (std::max(0, left) >= right || std::max(0, top) >= bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::Dot, left, top, left + dotWidth, top + dotHeight);
	_gpuEffects->boxes.back() = { std::max(0, left), std::max(0, top), right, bottom }; // (for the bins: what of it is in the view)
	texels[1] = (float)flags;
	texels[2] = (float)color;
	texels[3] = (float)depth;
	texels[8] = leeway; texels[9] = (float)otherColor; texels[10] = firstShare; texels[11] = haze;
}

/**
 * Records a round dot in front of the scene (a shot's head, a flare; a bubble's ring).
 * Texel 0: kind, flags (1 it glows, 2 measured from the pixels' middles, 4 only its rim, a pixel wide), its rim's
 * colour, its depth. Texel 2: its middle on the screen, its radius, its leeway in front of the scene; 3: its centre's
 * colour and the ring's round it, and how far out (of the radius) each reaches.
 */
void FirstPersonView::gpuRecordDisc(double screenX, double screenY, double radius, double depth, float leeway, Uint8 rim, Uint8 centre, Uint8 middle, float centreShare,
	float middleShare, int flags)
{
	const int reach = (int)std::ceil(radius) + 2;
	const int left = std::max(0, (int)screenX - reach), top = std::max(0, (int)screenY - reach), right = std::min(getWidth(), (int)screenX + reach + 1),
		bottom = std::min(getHeight(), (int)screenY + reach + 1);
	if (left >= right || top >= bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::Disc, left, top, right, bottom);
	texels[1] = (float)flags;
	texels[2] = (float)rim;
	texels[3] = (float)depth;
	texels[8] = (float)screenX; texels[9] = (float)screenY; texels[10] = (float)radius; texels[11] = leeway;
	texels[12] = (float)centre; texels[13] = (float)middle; texels[14] = centreShare; texels[15] = middleShare;
}

/**
 * Records a filled quad at one depth (fillQuadAtDepth).
 * Texel 0: kind, 0, its colour, its depth. Texels 2 and 3: its four corners on the screen.
 */
void FirstPersonView::gpuRecordQuad(const double corners[4][2], Uint8 color, float depth)
{
	double low[2] = { corners[0][0], corners[0][1] }, high[2] = { corners[0][0], corners[0][1] };
	for (int corner = 1; corner < 4; ++corner)
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			low[axis] = std::min(low[axis], corners[corner][axis]);
			high[axis] = std::max(high[axis], corners[corner][axis]);
		}
	}
	const int left = std::max(0, (int)std::floor(std::max(-1e6, low[0])) - 1), top = std::max(0, (int)std::floor(std::max(-1e6, low[1])) - 1),
		right = std::min(getWidth(), (int)std::ceil(std::min(1e6, high[0])) + 1), bottom = std::min(getHeight(), (int)std::ceil(std::min(1e6, high[1])) + 1);
	if (left >= right || top >= bottom)
	{
		return;
	}
	float *texels = _gpuEffects->add(FirstPersonGpu::Effects::Quad, left, top, right, bottom);
	texels[2] = (float)color;
	texels[3] = depth;
	for (int corner = 0; corner < 4; ++corner)
	{
		texels[8 + corner * 2] = (float)corners[corner][0];
		texels[9 + corner * 2] = (float)corners[corner][1];
	}
}

/**
 * Records the outlines round the units' pictures (settings "Hitboxes" with "Full sprite hitboxes";
 * see noteTestedPictures, which worked out each outlined unit's card and its box on the screen, and
 * drawUnitOutlines): they are drawn after everything recorded so far, in a second go on the card,
 * from the first one's pictures. A record for each picture (Effects::OutlineTexels texels):
 * 0: its box on the screen (its last column and row in it); 1: its anchor from the eye, in voxels, and one over its
 * column vector's length squared; 2: one picture column to the right, and one over its row vector's length squared;
 * 3: one picture row up, and the anchor's column; 4: the card's normal, and the anchor's row; 5: how many times finer
 * the picture tested is (drawn smooth up close), whether its rows below the feet count, where its bytes are, its width;
 * 6: its height, its outline's number, its outline's colour.
 */
void FirstPersonView::gpuRecordOutlines()
{
	GpuEffectList &list = *_gpuEffects;
	if (_outlinedUnits.empty() || list.effects.outlineAt >= 0)
	{
		return;
	}
	auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	for (const OutlinedUnit &outlined : _outlinedUnits)
	{
		const FirstPerson::SpriteCard &card = outlined.card;
		if (!card.picture)
		{
			continue;
		}
		int pictureAt, pictureWidth, pictureHeight;
		if (card.smooth)
		{
			const SDL_Surface *surface = const_cast<Surface*>(card.smooth)->getSurface(); // read only
			pictureWidth = surface->w;
			pictureHeight = surface->h;
			pictureAt = atlasPlace((const Uint8 *)surface->pixels, surface->pitch, pictureWidth, pictureHeight, nullptr);
		}
		else
		{
			pictureWidth = pictureHeight = SpriteBufferSize;
			pictureAt = atlasPlace(card.picture->data(), SpriteBufferSize, SpriteBufferSize, SpriteBufferSize, nullptr);
		}
		const double numbers[FirstPersonGpu::Effects::OutlineTexels * 4] = {
			(double)outlined.left, (double)outlined.top, (double)outlined.right, (double)outlined.bottom,
			card.anchor[0] - _eye[0], card.anchor[1] - _eye[1], card.anchor[2] - _eye[2], 1.0 / dot(card.column, card.column),
			card.column[0], card.column[1], card.column[2], 1.0 / dot(card.row, card.row),
			card.row[0], card.row[1], card.row[2], (double)card.anchorX,
			card.normal[0], card.normal[1], card.normal[2], (double)card.anchorY,
			card.smooth ? (double)card.factor : 1.0, card.rowsBelow ? 1.0 : 0.0, (double)pictureAt, (double)pictureWidth,
			(double)pictureHeight, (double)outlined.outline, (double)_outlineColors[outlined.outline - 1], 0.0 };
		for (const double number : numbers) list.outlines.push_back((float)number);
		++list.effects.outlineCount;
	}
	if (list.effects.outlineCount > 0)
	{
		list.effects.outlineAt = (int)list.boxes.size();
		list.effects.outlineOpacity = _outlineOpacity;
		list.effects.outlineSlack = _testedPictureSlack;
		list.effects.outlineGlows = Options::fpsGlowingOutlines;
	}
}

/**
 * Hands the recorded units and effects to the card, which draws them over the rays' pictures and
 * gives the finished ones back: the colour, the depth and the glow into the view's buffers, the
 * veils and whose picture shows round the pointer from its notes. Recording stops.
 * @return False if the card failed: the frame's rays are cast again on the CPU (what was recorded is lost for this
 *         one frame; the CPU draws everything from the next).
 */
bool FirstPersonView::gpuDrawEffects()
{
	_gpuRecording = false;
	GpuEffectList &list = *_gpuEffects;
	FirstPersonGpu::Effects &effects = list.effects;
	const int width = getWidth(), height = getHeight();
	const int bin = FirstPersonGpu::Effects::Bin;
	effects.width = width;
	effects.height = height;
	effects.primCount = (int)list.boxes.size();
	effects.prims.insert(effects.prims.end(), list.outlines.begin(), list.outlines.end()); // (the outlined pictures' records after the primitives)
	effects.binColumns = (width + bin - 1) / bin;
	effects.binRows = (height + bin - 1) / bin;
	// The bins: how many primitives touch each, then each one's list, in the order recorded.
	const int binCount = effects.binColumns * effects.binRows;
	list.binCounts.assign(binCount + 1, 0);
	for (const auto &box : list.boxes)
	{
		for (int row = box[1] / bin; row <= (box[3] - 1) / bin; ++row)
			for (int column = box[0] / bin; column <= (box[2] - 1) / bin; ++column) ++list.binCounts[row * effects.binColumns + column];
	}
	size_t entries = 0;
	for (int which = 0; which < binCount; ++which) entries += list.binCounts[which];
	effects.bins.resize(binCount + 1 + entries);
	Uint32 next = (Uint32)(binCount + 1);
	for (int which = 0; which < binCount; ++which)
	{
		effects.bins[which] = next;
		next += (Uint32)list.binCounts[which];
		list.binCounts[which] = (int)effects.bins[which]; // (from here: where its next entry goes)
	}
	effects.bins[binCount] = next;
	for (size_t number = 0; number < list.boxes.size(); ++number)
	{
		const auto &box = list.boxes[number];
		for (int row = box[1] / bin; row <= (box[3] - 1) / bin; ++row)
			for (int column = box[0] / bin; column <= (box[2] - 1) / bin; ++column) effects.bins[list.binCounts[row * effects.binColumns + column]++] = (Uint32)number;
	}
	// The camera, as the CPU's routines have it.
	for (int axis = 0; axis < 3; ++axis)
	{
		effects.forward[axis] = (float)_forward[axis];
		effects.right[axis] = (float)_right[axis];
		effects.up[axis] = (float)_up[axis];
		effects.levelRight[axis] = (float)_levelRight[axis];
		effects.levelUp[axis] = (float)_levelUp[axis];
	}
	effects.level[0] = (float)std::cos(_yaw);
	effects.level[1] = (float)std::sin(_yaw);
	effects.eyeZ = (float)_eye[2];
	effects.focal = (float)((width / 2.0) / _halfFovTangent);
	effects.seeThrough = _seeThroughTables.empty() ? nullptr : _seeThroughTables.data();
	effects.paletteHash = _colors.paletteHash;
	// The bloom after them, as drawLightHalos sets it up.
	effects.halos = !_nearestRgb.empty();
	effects.glowGain = _colors.underwater ? 45.0f : 18.0f + 42.0f * _colors.night;
	effects.maxDistance = _maxDistance;
	effects.palette = getPalette();
	effects.nearestRgb = _nearestRgb.empty() ? nullptr : _nearestRgb.data();
	// What comes back beside the picture: in the check every pixel's notes; else only those round the pointer (whose
	// picture the player points at).
	const bool allNotes = _gpuChecking;
	list.notes.resize(allNotes ? (size_t)width * height * 4 : 0);
	effects.notes = allNotes ? list.notes.data() : nullptr;
	const int patchLeft = std::max(0, _pointerX - PointerReach), patchTop = std::max(0, _pointerY - PointerReach),
		patchRight = std::min(width, _pointerX + PointerReach + 1), patchBottom = std::min(height, _pointerY + PointerReach + 1);
	const bool patched = !allNotes && patchLeft < patchRight && patchTop < patchBottom;
	effects.patch[0] = patchLeft; effects.patch[1] = patchTop;
	effects.patch[2] = patched ? patchRight - patchLeft : 0; effects.patch[3] = patched ? patchBottom - patchTop : 0;
	list.patchNotes.resize((size_t)(2 * PointerReach + 1) * (2 * PointerReach + 1) * 4);
	effects.patchNotes = patched ? list.patchNotes.data() : nullptr;

	if (_gpuChecking)
	{
		gpuCheckEffects();
		return true;
	}
	FirstPersonGpu::Output output;
	output.color = getRaw(0, 0);
	output.colorPitch = getPitch();
	output.depth = _depthBuffer.data();
	output.glow = _glowMask.data();
	output.results = nullptr;
	output.perRay = false;
	const bool fullColor = _wantsTrueColor && _gpuRaysPixelSize == 1;
	if (fullColor)
	{
		_trueColor.resize((size_t)width * height);
		_rayColors.resize(_trueColor.size());
	}
	output.trueColor = fullColor ? _trueColor.data() : nullptr;
	// Beside the picture: in full colour the rays' own palette colours (a pixel that still has its ray's is shown in
	// the full one, see trueColor), and with light in the air where the smoke lies; the depth only if asked later.
	const bool shaftSmoke = fullColor && _hasLightShafts;
	const bool ownColours = _nightVision && Options::fpsNightVisionUnits; // (futuristic night vision: the wash leaves the units', bodies' and items' colours)
	effects.rayColors = fullColor ? _rayColors.data() : nullptr;
	effects.readGlow = shaftSmoke || ownColours;
	effects.readDepth = false;
	FirstPersonGpu *gpu = readyFirstPersonGpu();
	// (OPENXCOM3D_GPU_TIMING: the effects' share of the card's time, in the log every 60 frames.)
	static const bool timed = getenv("OPENXCOM3D_GPU_TIMING") != nullptr;
	FirstPersonGpu::Timing timing;
	if (!gpu || !gpu->drawEffects(effects, output, timed ? &timing : nullptr))
	{
		runOnAllThreads(&FirstPersonView::renderRows);
		return false;
	}
	if (timed && width >= 320)
	{
		static double upload = 0.0, draw = 0.0, read = 0.0;
		static int frames = 0;
		upload += timing.upload; draw += timing.draw; read += timing.read;
		if (++frames == 60)
		{
			Log(LOG_INFO) << "OpenXcom3D GPU: the " << width << "x" << height << " view's units and effects (" << list.boxes.size() << " primitives, " << effects.bytes->size()
				<< " bytes of sprites kept): sending " << upload / frames << " ms, drawing " << draw / frames << " ms, reading back " << read / frames << " ms";
			upload = draw = read = 0.0;
			frames = 0;
		}
	}
	// The notes: the veils, the own colours, and whose picture shows round the pointer.
	for (int row = 0; allNotes && row < height; ++row)
	{
		const Uint32 *notes = &list.notes[(size_t)row * width * 4];
		Uint8 *veil = &_unitVeil[(size_t)row * width], *smoke = &_smokeVeil[(size_t)row * width], *own = &_ownColours[(size_t)row * width];
		for (int column = 0; column < width; ++column, notes += 4)
		{
			veil[column] = (Uint8)notes[0];
			own[column] = (Uint8)(notes[1] & 1);
			smoke[column] = (Uint8)(notes[1] >> 1);
		}
	}
	for (int patchY = 0; patchY <= 2 * PointerReach; ++patchY)
	{
		for (int patchX = 0; patchX <= 2 * PointerReach; ++patchX)
		{
			const int pixelX = _pointerX - PointerReach + patchX, pixelY = _pointerY - PointerReach + patchY;
			if (pixelX < 0 || pixelY < 0 || pixelX >= width || pixelY >= height) continue;
			if (!allNotes && !patched) continue;
			const Uint32 *notes = allNotes ? &list.notes[((size_t)pixelY * width + pixelX) * 4]
				: &list.patchNotes[((size_t)(pixelY - patchTop) * effects.patch[2] + (pixelX - patchLeft)) * 4];
			if (notes[2] && notes[2] <= list.pictured.size())
			{
				_pointerUnits[patchY][patchX] = list.pictured[notes[2] - 1].unit;
				_pointerBodies[patchY][patchX] = list.pictured[notes[2] - 1].body;
			}
			if (notes[3] && notes[3] <= list.pictured.size())
			{
				_pointerPiles[patchY][patchX] = list.pictured[notes[3] - 1].pile;
			}
		}
	}
	// In full colour with light in the air (setting "Light shafts"), the mode lays that light on see-through smoke:
	// where smoke lies over a pixel is the glow's bit 64.
	if (shaftSmoke)
	{
		for (size_t index = 0; index < (size_t)width * height; ++index)
		{
			_smokeVeil[index] = (_glowMask[index] & 64) ? 1 : 0;
		}
	}
	// ... and which pixels keep their own colours under the night vision's wash is its bit 32.
	if (ownColours)
	{
		for (size_t index = 0; index < (size_t)width * height; ++index)
		{
			_ownColours[index] = (_glowMask[index] >> 5) & 1;
		}
	}
	if (fullColor)
	{
		_hasTrueColor = true;
	}
	_gpuDepthPending = true; // (the depth stays on the card until something drawn after this asks: see gpuNeedDepth)
	return true;
}

/**
 * The depth of the frame's finished picture, fetched from the card the first time something drawn
 * on the CPU after the units and effects needs it (a downed unit's icon, a damage number, the
 * aimed path under night vision); most frames nothing does, and it is never read back.
 */
void FirstPersonView::gpuFetchDepth()
{
	_gpuDepthPending = false;
	FirstPersonGpu *gpu = readyFirstPersonGpu();
	if (!gpu || !gpu->readEffectsDepth(getWidth(), getHeight(), _depthBuffer.data()))
	{
		std::fill(_depthBuffer.begin(), _depthBuffer.end(), 0.0f); // (the card failed: everything counts as hidden this frame)
	}
}

/**
 * The check (OPENXCOM3D_GPU_EFFECTS=check): the CPU drew the passes as ever, and the frame shown
 * is its own; the card draws what was recorded beside it, and the two are compared, a pixel at a
 * time: the colour, the depth, the glow, the veils. The log gets the sums every 60 frames of the
 * main view; OPENXCOM3D_GPU_DUMP=folder gets the last compared frame's two pictures (palette
 * colours, a byte a pixel: cpu.bin, gpu.bin; size.bin; palette.bin).
 */
void FirstPersonView::gpuCheckEffects()
{
	GpuEffectList &list = *_gpuEffects;
	const int width = getWidth(), height = getHeight();
	const size_t pixels = (size_t)width * height;
	std::vector<Uint8> colors(pixels), glow(pixels);
	std::vector<float> depths(pixels);
	FirstPersonGpu::Output output;
	output.color = colors.data();
	output.colorPitch = width;
	output.depth = depths.data();
	output.glow = glow.data();
	output.results = nullptr;
	output.trueColor = nullptr;
	output.perRay = false;
	list.effects.readDepth = list.effects.readGlow = true;
	list.effects.rayColors = nullptr;
	FirstPersonGpu *gpu = readyFirstPersonGpu();
	if (!gpu || !gpu->drawEffects(list.effects, output, nullptr) || width < 320)
	{
		return;
	}
	static long frames = 0, checked = 0, differentColors = 0, differentDepths = 0, differentGlow = 0, differentVeils = 0, primitives = 0;
	for (int row = 0; row < height; ++row)
	{
		const Uint8 *cpuColors = getRaw(0, row);
		for (int column = 0; column < width; ++column)
		{
			const size_t index = (size_t)row * width + column;
			const Uint32 *notes = &list.notes[index * 4];
			differentColors += cpuColors[column] != colors[index];
			differentDepths += std::abs(_depthBuffer[index] - depths[index]) > 0.01f * std::max(1.0f, std::min(_depthBuffer[index], 1e6f));
			differentGlow += _glowMask[index] != (glow[index] & 3);
			differentVeils += _unitVeil[index] != (Uint8)notes[0] || _ownColours[index] != (notes[1] & 1);
		}
	}
	checked += (long)pixels;
	primitives += (long)list.boxes.size();
	if (++frames % 60 == 0)
	{
		Log(LOG_INFO) << "OpenXcom3D GPU: units and effects, " << primitives / 60 << " primitives a frame: of " << checked << " pixels " << differentColors << " colours differ ("
			<< 100.0 * differentColors / checked << "%), " << differentDepths << " depths, " << differentGlow << " glows, " << differentVeils << " veils";
		checked = differentColors = differentDepths = differentGlow = differentVeils = primitives = 0;
		if (const char *folder = getenv("OPENXCOM3D_GPU_DUMP"))
		{
			auto write = [&](const char *name, const void *data, size_t size) {
				if (FILE *file = fopen((std::string(folder) + "/" + name).c_str(), "wb"))
				{
					fwrite(data, 1, size, file);
					fclose(file);
				}
			};
			std::vector<Uint8> cpu(pixels), palette(768);
			for (int row = 0; row < height; ++row) std::memcpy(&cpu[(size_t)row * width], getRaw(0, row), width);
			const SDL_Color *shown = getPalette();
			for (int index = 0; index < 256; ++index)
			{
				palette[index * 3] = shown[index].r; palette[index * 3 + 1] = shown[index].g; palette[index * 3 + 2] = shown[index].b;
			}
			const int size[2] = { width, height };
			write("effects_size.bin", size, sizeof(size));
			write("effects_cpu.bin", cpu.data(), pixels);
			write("effects_gpu.bin", colors.data(), pixels);
			write("effects_palette.bin", palette.data(), 768);
		}
	}
}

}
