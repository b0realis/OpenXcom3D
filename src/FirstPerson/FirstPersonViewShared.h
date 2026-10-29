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
 *  Date: 2026-10-19
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
 * FirstPersonViewShared.h: the constants and small helpers that the FirstPersonView*.cpp files
 * share (proportions of the voxel world, timings, dithering, colour shading, noise, the tiny
 * fonts). Only those files include it (everything here is private to each of them).
 */
#include <algorithm>
#include <cmath>
#include <limits>
#include <SDL.h>
#include "../Battlescape/Position.h"

namespace OpenXcom
{

namespace
{

// ---- Numbers ----

const double Pi = 3.14159265358979323846;
/// The size of one tile in voxels on each axis: 16 across (x, y), 24 up (z).
const float CellSize[3] = { (float)Position::TileXY, (float)Position::TileXY, (float)Position::TileZ };
const float Infinity = std::numeric_limits<float>::infinity();

// ---- Sprites ----

/// Unit and item sprites are 32x40 isometric frames; this buffer fits a 2x2 unit.
const int SpriteBufferSize = 64;
/// Sprite upscaling (an option): how much bigger the smooth copy is (xBRZ), and from how many
/// screen pixels per sprite pixel on it is used (further away the original looks as good).
const int UpscaleFactor = 4;
const double UpscaleFrom = 1.6;
/// A thrown item flies as its voxel model (its inventory picture given volume): this many voxels
/// per picture pixel (a grenade's picture, about 12 pixels, is a third of a tile across),
/// turning end over end this many times a second.
const double ThrownItemScale = 0.35, ThrownItemSpin = 1.6;

// ---- Timings and limits ----

/// How long doors take to open in the view, in milliseconds, so you see them move (the game's
/// own doors open at once): a door swinging on its hinge, a UFO door sliding aside.
const Uint32 DoorSwingTime = 500, UfoDoorTime = 600;
/// The rounds that landed since the view's last frame (Map::noteShotLanded) are caught up on only if
/// that frame was at most this many milliseconds ago; a view that wasn't drawing for longer (the map
/// shown, the aliens' turn watched there, a screen on top) starts from now, or every round that
/// landed meanwhile would flash its sparks and beam at once (see FirstPersonView::followShots).
const Uint32 LandedShotsCatchUpTime = 1000;
/// Animated terrain (water, machinery, the lights and lit floors of UFOs) steps to its next
/// frame every CalmFrameTime milliseconds in the view, cross-fading over the whole step. The
/// game steps every 100 ms, which up close made whole lit panels blink five to ten times a
/// second: a photosensitivity hazard (WCAG 2.3 allows at most three flashes a second). At this
/// pace a two-frame blink is a soft pulse of about 1.6 Hz.
const Uint32 CalmFrameTime = 320;
/// The marks on a UFO's tiles in FirstPersonView::_ufoPhaseOfTile, above the pulse phase (bits
/// 0-3): a tile of the UFO, and one of its outer skin (beside a tile that isn't the UFO's).
const Uint8 UfoTileBit = 0x80, UfoSkinBit = 0x40;
/// How strongly a UFO's light colours its inside (its tiles under its roof), per unit of its
/// glow there (0.12 low down inside, see findUfos): by day, and more on top of it at night. So
/// the inside takes about a fifth of the light's colour by day, two fifths at night (an even
/// tint, see FirstPersonView::shade), where its hull outside keeps its detail with less.
const float UfoInsideTint = 1.8f, UfoInsideTintNight = 1.5f;
/// Tactical flares: how long they burn (seconds), how far their light reaches (voxels), and how
/// many can burn at once.
const float FlareLife = 60.0f, FlareReach = 150.0f;
const size_t FlareMax = 8;
/// Pixel size Auto (the option at 0): views up to this size get full detail, bigger ones blocks
/// of 2. A 1920x1080 screen at the default battlescape scale makes a 960x540 view, which ran at
/// 40-51 FPS in a busy town; in blocks of 2 it holds 60 and looks almost the same.
const int AutoPixelMaxWidth = 640, AutoPixelMaxHeight = 400;

// ---- Proportions of the voxel world ----

/// Top of the ground-level floor, in voxels (floors are solid in voxel layers 0-1).
const float GroundTop = 2.0f;
/// The discreet map edge (setting): a small red dot on the floor along the edge every this many
/// voxels (16 to a tile), this far inside the edge, this big across (voxels).
const float EdgeDotSpacing = 16.0f, EdgeDotInset = 3.0f, EdgeDotSize = 2.5f;
/// X-COM's voxels aren't cubes. The isometric art is drawn in 2:1 dimetric with one pixel per
/// voxel up, where a true projection of cubes would draw 1.22 pixels: one voxel up is sqrt(2/3)
/// of one voxel across. The camera works in true proportions; rays walk the voxel grid.
const double ZScale = 0.81649658092772603;
const float ZScaleF = (float)ZScale;
/// An isometric sprite pixel across is 1/sqrt(2) of a voxel (32 pixels span a tile's diagonal).
const double SpritePixelX = 0.70710678118654752;
/// Sprites stand like cards facing the camera, leaning with the view's pitch; looking down
/// further than this many degrees, a card leans only this far, so it stays above the floor
/// instead of lying in it (see drawSprite).
const double CardTiltMax = 45.0;
/// Looking up, a card leans only this far (its top comes towards the camera): a unit right
/// beside the soldier leaning in by much more lurched at the camera.
const double CardTiltUpMax = 10.0;
/// A card standing high above the eye also leans towards the eye, by how much steeper than
/// CardTiltUpMax it is, at most this far: a unit straight overhead faces down at the soldier
/// instead of showing its edge (see drawSprite).
const double CardTiltSteepMax = 80.0;
/// How far the view can look up or down, in degrees: all but straight up and down (the camera
/// keeps its heading there).
const double MaxPitch = 89.0;
/// See-through smoke over a unit the soldier sees veils it by at most this many see-through steps
/// (of FirstPersonView::SeeThroughLevels) in all, however much smoke lies in front: it stays clear.
const int UnitVeilMax = 5;
/// The see-through smoke keeps this share of its colours (the rest turned to grey; see softSmokeFrame).
const float SmokeColorKept = 0.35f;
/// The thinnest see-through smoke's opacity the clouds are made for (Modern; thicker smoke more,
/// see drawSeeThroughSmoke): the settings "Smoke opacity" and "Night smoke opacity" scale every
/// cloud and wisp by theirs over this one (45% by day: the look before the settings).
const float SmokeOpacityMadeFor = 0.45f;
/// Where the night value takes over by day, by the game's shade where the smoke hangs (0
/// brightest .. 15 darkest): the day value up to the first, the night value from the second,
/// blended between (an alien base's dark rooms, an unlit room). On a night mission the night
/// value holds anyway (the battle's darkness, on the same scale: see smokeOpacityScale).
const float SmokeNightFromShade = 3.0f, SmokeNightToShade = 12.0f;
/// Eyes with heat vision (the armor's heatVision, in percent: how much of the smoke they see
/// through, by the game's sight rules) see the see-through smoke that much thinner (a player: a
/// scout drone's 50% heat vision didn't look any better), but never less than this share of it,
/// so smoke stays in sight (see FirstPersonView::setSmokeSight).
const float SmokeSeenMin = 0.2f;
/// A lying sprite's rows below its anchor lie on the floor, each this many voxels nearer the
/// camera: one isometric pixel down is sqrt(2) voxels along the floor towards the viewer.
const double LyingRowVoxels = 1.41421356237309505;
/// Things on the ground are drawn standing on the floor slab the view builds (two voxels thick),
/// raised out of whatever solid they are in by at most this many voxels (see standingHeight).
const int StandRaiseMax = 4;
/// The bloom over a pixel too dark to show its colour group's hue (palette luminance, 0..255,
/// below DarkGlowLuma: night's black, the void past the map) adds the glow's own colour, as much
/// of it as brightens by DarkGlowStepLuma for each shade of brightening, about what a shade up a
/// dark colour group does (see FirstPersonView::drawLightHalos); a brighter pixel goes up its own
/// colour group as before.
const float DarkGlowLuma = 28.0f;
const float DarkGlowStepLuma = 10.0f;
/// A floor the art draws as a box of liquid (XPiratez's sewers under its bases: a green block about
/// half a level tall, though the game gives it only the floor's thin slab of voxels and slows
/// walking through it; a player: "very intentionally about thigh deep"): the art's outline must be
/// a tile-wide box this many voxels tall, LiquidMinHeight to LiquidMaxHeight, matching it on this
/// share of their pixels together (see liquidHeightOf), and walking it must cost more time units
/// than a plain floor.
const int LiquidMinHeight = 6, LiquidMaxHeight = 20;
const float LiquidOutlineMatch = 0.97f;
const int PlainFloorWalkCost = 4;
/// The floor under such a liquid is drawn this many shades darker than the liquid's top, so its
/// surface (a ghost, see buildLiquid) reads as a layer above it.
const int LiquidBottomShades = 3;
/// A floor or wall whose voxels leave out most of its art (a catwalk's grating given a few voxels a
/// layer, a railing posts on every other layer, so shots go through: see sparseArt): its voxels all
/// within ArtFillPlaneDepth voxels of its plane (a floor's bottom, a wall's edge), filling under
/// ArtFillShare of the cells of that plane its art draws, of which there are ArtFillMinCells at least
/// (not a scrap of trim).
const int ArtFillPlaneDepth = 4;
const float ArtFillShare = 0.5f;
const int ArtFillMinCells = 40;
/// The floor a lying sprite's rows lie on (items, corpses) rises towards the camera with the ground
/// in front of it, a step or a slope (see lyingRise), so they lie on it rather than in it (a
/// player's report: items on stairs and slopes sank into them): the ground looked for up to
/// LyingRiseMax voxels above the item's floor, the slope at most LyingSlopeMax (voxels up for each
/// voxel towards the camera).
const int LyingRiseMax = 12;
const double LyingSlopeMax = 1.5;
/// Something lying on sloping ground (a ramp, the steps of stairs; a player: items there, seen
/// from the side, sank half into it) lies level at the ground's height under its middle, drawn
/// over the steps round it (the owner: "just on top of stairs at half height"; see
/// FirstPersonView::groundPlane): the plane through the ground's surface at 5x5 points round it,
/// GroundPlaneReach voxels out from it each way (a lone item's within its tile, a big corpse's
/// over its four), the surface looked for from GroundPlaneProbe voxels above the item's floor
/// down to as far below, the plane then lifted onto the highest of those points (resting on the
/// edges of steps) by at most GroundPlaneLiftMax; the depth test gives way by the ground's rise
/// within that reach, at most SlopeItemDepthBiasMax voxels (more would show it through a wall).
const double GroundPlaneReach = 6.0, GroundPlaneReachCorpse = 12.0, SlopeItemDepthBiasMax = 6.0;
const int GroundPlaneProbe = 12, GroundPlaneLiftMax = 4;
/// Bullet holes and scorch marks (see drawDecals): a mark's radius, in voxels, per step of its
/// size (a bullet hole is size 1, a scorch 3); at most this many pixels from its middle on the
/// screen, however close; a bullet hole's core this many shades darker (its edge by the mark's
/// own darkness, 4), so it shows on busy textures such as the desert's sand.
const double MarkRadius = 0.3;
const int MarkMaxPixels = 6;
const int BulletHoleCoreShades = 7;
/// Unexplored terrain turns misty with distance (see shadeHit): blended towards the haze from
/// UnexploredMistFrom to UnexploredMistTo times the game's sight range (10 to 30 tiles at its
/// usual 20), at most UnexploredMistMax of the way, so far shapes are a soft blue-grey, never
/// sky-coloured boxes; the distance haze still takes them fully at the view's reach.
const float UnexploredMistFrom = 0.5f, UnexploredMistTo = 1.5f, UnexploredMistMax = 0.6f;
/// The sea's waves (the mod's water, TFTD's sea: see FirstPersonView::waveHeight): trains of waves
/// crossing, each its length from crest to crest (voxels, 16 to a tile), its heading (radians
/// from east towards south), its speed (voxels a second) and its share of the height (together
/// 1). Each flattens out from fadeFrom to fadeTo voxels away, before its lines grow finer than the
/// pixels and shimmer. Its crests bend along their length, bendDepth voxels to and fro over
/// bendLength (straight crests crossing drew a regular lattice of diamonds further out).
struct WaveTrain { float length, heading, speed, share, fadeFrom, fadeTo, bendLength, bendDepth; };
const WaveTrain WaveTrains[] = {
	{ 56.0f, 0.6f, 11.0f, 0.5f, 320.0f, 640.0f, 173.0f, 11.0f },  // the swell, three and a half tiles long
	{ 31.0f, -0.4f, 8.0f, 0.32f, 200.0f, 400.0f, 109.0f, 7.0f },  // a second swell across it
	{ 17.0f, 2.4f, 5.0f, 0.18f, 100.0f, 200.0f, 61.0f, 4.0f } }; // the chop, about a tile long, running against them
const int WaveTrainCount = (int)(sizeof(WaveTrains) / sizeof(WaveTrains[0]));
/// A wave's crest is WaveShades lighter than the sea's plain colour and its trough as much darker;
/// above FoamFrom of the height (-1 .. 1) a crest breaks into foam (the plain colour FoamShades
/// lighter), more of it towards the top; below -DeepFrom a trough takes the sea's deep colour.
const float WaveShades = 2.0f, FoamFrom = 0.5f, DeepFrom = 0.3f;
const int FoamShades = 2;
/// The sea is shaded no darker than this shade of its colour group (0 brightest .. 15 darkest)
/// unless the place is really dark (see DarkOverflowFrom): the last shades of TFTD's sea colours
/// are all but black, and its troughs at dusk were black blotches.
const int WaterDarkestShade = 12;
/// The sine the waves are made of is a table of this many steps a turn: cheap for every pixel of sea.
const int WaveSineSteps = 1024;
/// The purple haze of a unit under our mind control (see FirstPersonView::setControlHaze) creeps in
/// from this share of the way from the middle of the view to its corners, fully at the corners.
const float ControlHazeFrom = 0.45f;
/// The units' facing rings (see FirstPersonView::drawFacing): the arrow reaches this many voxels out
/// from the ring, this many to each side at its base.
const double FacingArrowLength = 7.0, FacingArrowWidth = 4.0;
/// A rocket's or a cannon shell's lasting trail (see spawnTrailSegment): its white specks hang this
/// many seconds, at random between the two (a player: they lasted "far, far too long" at 25 to 35,
/// "cut their particle lifetime down 2 thirds"; the owner: the specks only, not the smoke puffs).
const float TrailSpeckLifeLeast = 8.0f, TrailSpeckLifeMost = 12.0f;
/// A helmet camera's feed (the soldier cam, the map camera) gets a spell of TV static now and then,
/// not all the time (the owner's ask: "gradually from zero to lots of static and back down - the
/// duration random, the speed random and interval between static random - from 10s to 2 min"):
/// StaticGapShortest to StaticGapLongest ms after the last one, it lasts StaticSpellShortest to
/// StaticSpellLongest ms, rising to a peak of StaticPeakLeast to StaticPeakMost of the pixels and
/// falling back, each over StaticRampLeast to StaticRampMost percent of it, at random (see
/// FirstPersonView::updateStaticSpell). With a bad signal
/// (the hardest missions, inside a UFO: "more interference is expected") a low static all the time,
/// BadStaticFloor, and spells more often, longer and stronger (the Bad... values).
const int StaticGapShortest = 10000, StaticGapLongest = 120000;
const int StaticSpellShortest = 2500, StaticSpellLongest = 8000;
const float StaticPeakLeast = 0.35f, StaticPeakMost = 0.85f;
const int StaticRampLeast = 25, StaticRampMost = 45;
const float BadStaticFloor = 0.03f;
const int BadStaticGapShortest = 5000, BadStaticGapLongest = 40000;
const int BadStaticSpellShortest = 3000, BadStaticSpellLongest = 10000;
const float BadStaticPeakLeast = 0.5f, BadStaticPeakMost = 0.95f;
/// Under night vision the aimed path (see FirstPersonView::drawTrajectory) is light in the goggles,
/// as a laser sight is: its core this share white, the rest the goggles' brightest green (dithered:
/// a white-green, the owner's choice), in a green glare; a forced arc's dashes turn to dots
/// (colour can't tell it from a normal arc there), NightForcedDotSize pixels across, one every
/// NightForcedDotSpacing pixels along the path on the screen.
const float NightLightWhiteShare = 0.5f;
/// A straight shot the game will refuse (no line of fire; the owner's ask after a player's report)
/// shows its laser dimmed: dark red, this share of its pixels (dithered), its dot no brighter.
const float RefusedLineOpacity = 0.5f;
const int NightForcedDotSize = 2;
const float NightForcedDotSpacing = 8.0f;
/// A level of a map is underground at its edges when at least this share of its outer ring of
/// tiles is rock: a wall or an object (the rock a base or a cave is cut into), or a floor with
/// something over it (a base's sewers). 100% on every base, colony and Cydonia map seen; at
/// most 65% at ground level outdoors, trees, fences and roofed houses included.
const float UndergroundRingShare = 0.9f;
/// The rock round and over an underground map is mottled over this many shades (see rockColor),
/// from this shade of the colour group of the map's own rock (see chooseGround).
const int RockMottleShades = 3, RockBaseShade = 6;
/// A colour shaded past the darkest of its group turns black only where the place is really
/// dark (see shadeHit): the light there (the game's, less lamps, the flashlight and flares; 0
/// brightest .. 15 darkest) from DarkOverflowFrom (none of it, the group's darkest instead) to
/// DarkOverflowTo (all of it), dithered between. A lit house is 2, the Piratez hideout's rooms
/// about 4.5, a night field at the edge of a soldier's light about 6.5, far from it 15.
const float DarkOverflowFrom = 5.0f, DarkOverflowTo = 8.0f;
/// A ceiling under rock (see underRock) whose plain colour is this far down its group (its shade,
/// 0 brightest .. 15 darkest) or further shows its top's pattern, its middle shade stopping
/// RockMottleShades short of the darkest, as the rock beyond the map (see shadeHit): dark rock
/// sank into one flat darkest colour. Brighter art (a base's grey roof plates) keeps its plain
/// ceiling: its top's pattern is the iso art's shading.
const int DarkCeilingShade = 8;
/// A shot's mark goes where its line meets what the view draws, searched from this many voxels
/// back along the line (an object's art can stand out in front of its collision shape; see
/// spawnShotImpact).
const float ImpactSearchBack = 8.0f;
/// A shot that ends with no surface drawn along its line and nothing solid within this many voxels
/// of its end ended in the open (off into the sky, where the game's line leaves the map): no impact
/// at all (see spawnShotImpact).
const int ImpactSolidReach = 2;
/// A part without shape whose art is bars on the floor's two back edges is a cage, built on all
/// four edges (see cageArt): each drawn column ends at most this many rows below the back edges'
/// line, and there are at least CageMinColumns on each edge.
const int CageEdgeRows = 3, CageMinColumns = 2;

// ---- Colour helpers ----

/// Ordered dithering (a 4x4 Bayer matrix): a threshold per pixel, used to turn fractional
/// shades, washes and gradients into the 8-bit palette's whole colours without banding.
const float Bayer[4][4] = {
	{ 0 / 16.0f, 8 / 16.0f, 2 / 16.0f, 10 / 16.0f },
	{ 12 / 16.0f, 4 / 16.0f, 14 / 16.0f, 6 / 16.0f },
	{ 3 / 16.0f, 11 / 16.0f, 1 / 16.0f, 9 / 16.0f },
	{ 15 / 16.0f, 7 / 16.0f, 13 / 16.0f, 5 / 16.0f } };

/**
 * The dithering threshold (0 .. 15/16) for a pixel: something covering a fraction f of the
 * screen is drawn on the pixels where dither(x, y) < f.
 */
inline float dither(int x, int y) { return Bayer[y & 3][x & 3]; }

/**
 * Darkens a palette colour within its 16-colour group, like the game's StandardShade (each
 * group runs from bright to dark): anything that would spill into the next group becomes black.
 * @param color The palette index.
 * @param shade How many steps darker (0 or less: unchanged).
 */
inline Uint8 shadeColor(Uint8 color, int shade)
{
	if (shade <= 0)
	{
		return color;
	}
	const int shaded = color + shade;
	if (shaded > 255 || ((shaded ^ color) & 0xF0))
	{
		return 0x0F;
	}
	return (Uint8)shaded;
}

/**
 * A smooth step from 0 (at or below edgeLow) to 1 (at or above edgeHigh), easing in and out.
 */
inline float smoothstep(float edgeLow, float edgeHigh, float value)
{
	const float progress = std::clamp((value - edgeLow) / (edgeHigh - edgeLow), 0.0f, 1.0f);
	return progress * progress * (3.0f - 2.0f * progress);
}

// ---- Noise (clouds, flicker, scattering) ----

/**
 * A well-mixed hash of two integers: the same pair always gives the same pseudo-random bits.
 */
inline Uint32 hash2(int x, int y)
{
	Uint32 mixed = (Uint32)x * 374761393u + (Uint32)y * 668265263u;
	mixed = (mixed ^ (mixed >> 13)) * 1274126177u;
	return mixed ^ (mixed >> 16);
}

/**
 * Smooth value noise in [0, 1): a random value at every whole point of the plane, blended
 * smoothly in between.
 */
inline float valueNoise(float x, float y)
{
	// The grid square the point is in, and where in it (eased, so the blend has no creases).
	const int cornerX = (int)std::floor(x), cornerY = (int)std::floor(y);
	const float insideX = x - cornerX, insideY = y - cornerY;
	const float easeX = insideX * insideX * (3 - 2 * insideX), easeY = insideY * insideY * (3 - 2 * insideY);
	// The random values at its four corners, blended along x, then along y.
	auto cornerValue = [](int gridX, int gridY) { return (hash2(gridX, gridY) & 0xFFFF) / 65536.0f; };
	const float topLeft = cornerValue(cornerX, cornerY), topRight = cornerValue(cornerX + 1, cornerY);
	const float bottomLeft = cornerValue(cornerX, cornerY + 1), bottomRight = cornerValue(cornerX + 1, cornerY + 1);
	return (topLeft + (topRight - topLeft) * easeX) + ((bottomLeft + (bottomRight - bottomLeft) * easeX) - (topLeft + (topRight - topLeft) * easeX)) * easeY;
}

/**
 * Cloud-like noise: three layers of value noise, each finer and fainter than the last.
 */
inline float cloudNoise(float x, float y)
{
	return valueNoise(x, y) * 0.55f + valueNoise(x * 2.1f + 17.0f, y * 2.1f) * 0.3f + valueNoise(x * 4.3f, y * 4.3f + 31.0f) * 0.15f;
}

// ---- Tiny fonts, drawn into the world ----

/// 3x5 glyphs for the text on the yellow tape around the map border: rows top to bottom, each
/// row three bits with bit 2 the left column.
const Uint8 TapeFont[][5] = {
	{ 5, 5, 2, 5, 5 }, // X
	{ 0, 0, 7, 0, 0 }, // -
	{ 7, 4, 4, 4, 7 }, // C
	{ 7, 5, 5, 5, 7 }, // O
	{ 5, 7, 7, 5, 5 }, // M
	{ 0, 0, 0, 0, 0 }, // space
};
/// The tape's text, repeated along it, as glyphs of TapeFont: "X-COM  ".
const int TapeText[] = { 0, 1, 2, 3, 4, 5, 5 };
/// 3x5 digits for the numbered markers over the visible enemies, same layout as TapeFont.
/// A spray burst's marks (see FirstPersonView::drawSprayMarks): a dot of the line along them every
/// SprayDotSpacing voxels; each mark a pole SprayPoleVoxels high from the tile's floor (two thirds of a level; at least
/// SprayPoleMinPixels on the screen, far off), a mini target SprayTargetRadius pixels across on it
/// SprayAimVoxels up (the game aims the rounds at the tile's middle, waist high).
const double SprayDotSpacing = 4.0, SprayPoleVoxels = 16.0, SprayAimVoxels = 11.0;
const int SprayPoleMinPixels = 18, SprayTargetRadius = 3;
const Uint8 DigitFont[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 3, 1, 7 }, { 5, 5, 7, 1, 1 },
	{ 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 } };
const int TapeTextLength = sizeof(TapeText) / sizeof(TapeText[0]);

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
