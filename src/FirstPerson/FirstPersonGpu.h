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
 * FirstPersonGpu.h: the 3D view's rays on the graphics card.
 *
 * The CPU renderer walks one ray per pixel through the voxel world (castRay, in
 * FirstPersonViewRayWalk.h). This does the same walk in a fragment shader: the frame's tile
 * snapshot and the voxel models go up to the card as plain buffers, the shader is castRay and
 * traceTile line for line, and what each ray met comes back. The CPU renderer stays the
 * reference: whatever this draws is checked against it, pixel by pixel.
 *
 * It keeps a thread of its own with a hidden OpenGL context (GpuContext), so it never touches
 * the game's display, whichever way that draws.
 */
#include <SDL.h>
#include <memory>
#include <string>
#include "FirstPersonView.h"

namespace OpenXcom
{

class FirstPersonGpu;
/// The one renderer on the graphics card, once it is open and ready (see FirstPersonViewGpu.cpp); null before, and where there is none.
FirstPersonGpu *readyFirstPersonGpu();

class FirstPersonGpu
{
public:
	/// A wall whose hidden face takes another wall's paint (FirstPersonView::WallPaint).
	struct WallPaint
	{
		const FirstPersonView::VoxelModel *wall, *donor;
		bool donorWest, openEnd;
		Uint8 kind, paintGroup;
	};
	/// A craft's mirror (FirstPersonView::Mirror), its rows where the view keeps them.
	struct Mirror
	{
		int lo[3], size[3], sum[2];
		bool symmetric[2];
		const int *start[2];
		const Sint16 *pos[2], *outer[2];
		size_t startCount[2], posCount[2], outerCount[2];
	};
	/// What one frame's rays need: the world as the CPU's rays see it, and the camera.
	struct Frame
	{
		const FirstPersonView::TileView *tiles;   // the tile snapshot, one per tile
		const Uint8 *tileFlags;                   // the byte per tile the walk reads first
		const FirstPersonView::DoorSwing *swings; // the doors swinging this frame
		int swingCount;
		int size[3];                              // the map's size in tiles
		float maxDistance;                        // how far rays go, in voxels
		float eye[3], forward[3], right[3], up[3];
		float halfPlaneWidth, halfPlaneHeight;    // the image plane one voxel in front of the eye
		int width, height, pixelSize;             // the view in pixels, and how many pixels a ray colours each way
		float ghostScale;                         // setting "Ghost opacity" (see renderRows)
		bool seeThroughCrops;                     // setting "See-through crops"
		// The scene the shading reads (see FirstPersonView::shadeHit and colorOutsideMap).
		const FirstPersonView::SceneColors *colors; // this frame's palette colours and light
		const Uint8 *roofed;                      // a byte per tile: a roof over it (FirstPersonView::indoorsAt)
		const Uint8 *waterAtEdge;                 // a byte per tile of a level: the water reaches the map's edge there; null for none
		const FirstPersonView::VoxelModel *groundModel, *waterModel; // the ground and the water beyond the map (null: none)
		Uint8 waterColors[3], rockBase;
		int sky;                                  // FirstPersonView::Sky
		bool enclosed;
		int surfaceLevel;
		float waveSteps[18];
		float animationBlend;
		float sightView, sightMod;                // the game's sight ranges, in voxels (the tile engine's, the mod's)
		int fogOfWar, unseenTerrain, haze;        // the settings
		bool shadows, smoothLighting, discreetEdge;
		// The finer picture (settings, all off: the picture is the CPU's). See FirstPersonGpuShaders.h.
		int samples;                              // rays a pixel, blended: 1, 2 or 4 (setting "Supersampling")
		bool softShadows, allLightShadows, ambientOcclusion; // settings "Soft sun shadows", "Shadows from every light", "Ambient occlusion"
		const SDL_Color *palette;                 // the view's palette: what the samples' colours blend to (null: no blending)
		float colorLights[16][8];                 // setting "Coloured lights": each light's place (voxels), its reach, its colour (0..1 each) and strength
		int colorLightCount;                      // ... how many this frame (0: none, or the setting off)
		bool colorLightsOn;                       // ... the setting is on (the program for them is built ahead of the first light)
		bool reflections;                         // setting "Reflections": the sea mirrors the sky and what stands in it (in full colour)
		bool lightShafts, shaftSmoke;             // setting "Light shafts": this frame has light in smoke or water to show (in full colour); there is smoke on the map
		float shaftAir;                           // ... how much of the light a voxel of clear water throws towards the eye (0 above the sea)
		bool smoothLight;                         // setting "Smooth light": the full-colour picture blends where the palette's dithers
		float lamps[16][4];                       // the lamps nearby: position, strength
		int lampCount;
		int flashlight;                           // 0 off, 1 the shoulder flashlight, 2 the dynamic one
		float flashlightPos[3], forwardInVoxels[3];
		float flares[8][4], flareFlicker[8];      // the flares' lights: position, fade; flicker
		int flareCount;
		float muzzleFlashLight, muzzleFlashPos[3];
		// UFO lights (the option), crafts' mirrored sides and the wall paint (setting).
		const Uint8 *ufoOfTile, *ufoPhaseOfTile;  // a byte per tile each, or null
		bool ufoLightsOn;
		float ufoPulse[32], ufoBreath, ufoFaceShade[64];
		int ufoCount;                             // at most 8
		const Uint8 *ufoTint[8], *ufoSoftTint[8]; // each UFO's tint tables: 2 x 256 and 12 x 256 bytes
		Uint8 black[256];                         // each palette colour: all but black (see FirstPersonView::mirroredColor)
		int wallPaint;                            // the setting
		const Uint16 *wallPaintOf;                // two per tile (west wall, north wall): 1 + the paint's index, or null
		const WallPaint *wallPaints;
		int wallPaintCount;
		const Sint16 *mirrorOfTile;               // per tile: its craft's mirror, -1 none; or null
		const Mirror *mirrors;
		int mirrorCount;
	};
	/// What a ray met, as it comes back from the card (four numbers a ray).
	struct RayResult
	{
		Uint32 tile;  // the tile hit
		Uint32 bits;  // part (0..3, 7 a swinging door) | (axis + 1) << 4 | ghost << 8 | glass << 9 | emissive << 10 | how it ended (RayEnd) << 12 | the raw colour met << 16
		Uint32 voxel; // x | y << 8 | z << 16, in the tile
		float distance; // along the ray, to the hit or to where it left the map
	};
	/// Where a frame's picture goes: the view's own buffers, a ray's block of pixels filled as the CPU's rays fill it.
	struct Output
	{
		Uint8 *color;        // palette colours, rows from the top
		int colorPitch;      // bytes from one row of them to the next
		float *depth;        // how far along the view axis, width x height
		Uint8 *glow;         // 1 where the pixel shines, 3 a UFO's light; width x height
		RayResult *results;  // what each ray met (a ray each, not a pixel), for the check against the CPU's; null: not asked
		Uint32 *trueColor;   // the picture in full colour, a pixel each (0x00RRGGBB), width x height, rows from the top; null: not asked (setting "True colour")
		bool perRay;         // the buffers hold a ray each (the blocks' columns x rows), not a pixel each: nothing is spread over blocks
		int keep = 0;        // 1: nothing is read back, the pictures stay on the card for drawEffects, which fills these buffers when it is done; 2: read back as ever, and kept for it too (its check against the CPU's passes)
	};
	/// The frame's units and effects for the card: what the CPU's passes after the rays would draw, as primitives in
	/// their drawing order (see FirstPersonViewGpuEffects.cpp, which records them, and EffectsSource, which draws them).
	struct Effects
	{
		static const int Texels = 10;  // a primitive: this many texels of four floats
		static const int Bin = 32;     // the screen in squares of this many pixels: each has the list of the primitives that may touch it
		enum Kind { Sprite = 1, GroundShade, Decal, Line, Dot, Disc, Quad };
		std::vector<float> prims;      // the primitives
		const std::vector<Uint8> *bytes = nullptr; // the sprites' pixels and opacity maps, where the primitives say: kept from frame to frame, only added to,
		Uint32 bytesGeneration = 0;    // ... until they are begun anew (another number here): what the card has of them stays good till then
		std::vector<Uint32> bins;      // per bin (and one more) where its list starts in here; then the lists: a primitive's number each, in order
		int binColumns = 0, binRows = 0;
		float forward[3], right[3], up[3], levelRight[3], levelUp[3], level[2]; // the camera in true proportions (FirstPersonView::updateBasis)
		float eyeZ, focal;             // the eye's height, in voxels; the focal length, in pixels
		int width = 0, height = 0;     // the view, in pixels
		const Uint8 *seeThrough = nullptr; // a colour over another, by level: FirstPersonView::_seeThroughTables
		Uint32 paletteHash = 0;        // the palette they, and the two below, were made for (sent again when it changes)
		// The bloom after them (FirstPersonView::drawLightHalos).
		bool halos = false;            // drawn on the card too
		float glowGain = 0.0f;         // how strongly the glow brightens
		float maxDistance = 0.0f;      // the view's reach, in voxels (something shining in the sky counts as that far)
		const SDL_Color *palette = nullptr; // the view's palette
		const Uint8 *nearestRgb = nullptr;  // the palette colour nearest each 15-bit colour (FirstPersonView::_nearestRgb)
		// What comes back beside the picture (the colour always; each of these only if asked).
		bool readDepth = false;        // the depth, into Output::depth (else on demand: readEffectsDepth)
		bool readGlow = false;         // the glow (1, 3; 64: see-through smoke lies over the pixel), into Output::glow
		Uint8 *rayColors = nullptr;    // the palette colour each pixel had from its ray, before anything was drawn over it (for the full-colour picture)
		Uint32 *notes = nullptr;       // four numbers a pixel: the unit veil left, own colours | smoke veil << 1, 1 + the last solid sprite's number, 1 + the last pile's; null: not asked
		int patch[4] = {};             // ... or only those of a patch of the view (left, top, width, height; none wide: no patch), row by row from its top:
		Uint32 *patchNotes = nullptr;  // what the player points at is read from the notes round the pointer
	};
	/// Where a frame's time went, in milliseconds.
	struct Timing
	{
		double upload = 0.0, draw = 0.0, read = 0.0;
	};

	/// Opens the renderer on a graphics card (by number, where there are several). Null where it can't be had.
	static std::unique_ptr<FirstPersonGpu> open(int device);
	/// Opens it on the first graphics card that isn't a software one. Null where there is none.
	static std::unique_ptr<FirstPersonGpu> openAny();
	~FirstPersonGpu();
	/// The graphics card's name.
	const std::string &name() const;
	/// Builds ahead the programs that have the coloured lights (1), the reflections (2) and the light shafts (8) in them, and those for a
	/// pixel's later rays (4, "Supersampling"), for the settings that are on: else the first frame that needs one waits while the driver builds it.
	void prebuild(int extras);
	/// Forgets every model sent up so far (a new battle: the view builds its models anew).
	void forgetModels();
	/// Casts the frame's rays, a ray for each block of pixelSize x pixelSize pixels, and fills the picture.
	/// @param timing If not null, the card is waited for after each step and the times are noted (slower).
	/// @return False if the card failed (the caller keeps to the CPU).
	bool castRays(const Frame &frame, const Output &output, Timing *timing);
	/// Draws the frame's units and effects over the rays' pictures kept on the card (castRays with Output::keep), and
	/// reads the finished pictures back into the view's buffers.
	/// @return False if the card failed (the caller keeps to the CPU from now on).
	bool drawEffects(const Effects &effects, const Output &output, Timing *timing);
	/// The depth of the units and effects last drawn for a view of this size, read back after all (see Effects::readDepth).
	bool readEffectsDepth(int width, int height, float *depth);

private:
	FirstPersonGpu();
	struct State;
	std::unique_ptr<State> _state;
};

}
