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
 * FirstPersonView.cpp: the heart of the 3D view.
 *
 * Creating the view, the camera (eye, direction, field of view), and draw(), which renders one
 * frame: the tile snapshot, the ray casting on several threads, then sprites and effects, and
 * the colour wash over the whole screen.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <SDL.h>
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Sound.h"
#include "../Engine/Surface.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Battlescape/Position.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

/**
 * Creates the first-person view: an 8-bit surface the 3D picture is drawn into, with a 75
 * degree horizontal field of view to start with (see setFieldOfView), a depth buffer and a
 * glow mask of one entry per pixel, and a scratch surface big enough for one unit sprite
 * (a 2x2 unit's frames, see SpriteBufferSize). The battle comes later (setSavedBattleGame).
 * @param game Pointer to the core game.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 */
FirstPersonView::FirstPersonView(Game *game, int width, int height, int x, int y) :
	InteractiveSurface(width, height, x, y), _game(game), _halfFovTangent(std::tan(75.0 * Pi / 360.0)),
	_spriteBuffer(new Surface(SpriteBufferSize, SpriteBufferSize))
{
	// Per pixel: how far away the world seen there is (so sprites hide behind walls), whether
	// it shines (for the bloom), and how much see-through smoke may still veil a unit seen there.
	_depthBuffer.resize(width * height);
	_glowMask.resize(width * height);
	_unitVeil.resize(width * height);

	// A camera's glitches and static spells differ from game to game and from camera to camera
	// (the time it's made, and where).
	_glitchSeed ^= SDL_GetTicks() * 2654435761u ^ (Uint32)(uintptr_t)this;
}

/**
 * Cleans up the first-person view: frees its scratch surfaces, stops the sprite upscaling
 * worker thread (waiting for it to finish its current sprite), and frees the upscaled sprites
 * it made.
 */
FirstPersonView::~FirstPersonView()
{
	// The scratch surfaces sprites are drawn through.
	delete _spriteBuffer;
	for (Surface *frame : _composedFrames) delete frame;
	delete _itemPicture;
	for (Surface *buffer : _blendBuffers) delete buffer;
	for (auto &entry : _softSmoke) delete entry.second.colors;
	for (auto &entry : _stippleSmoke) delete entry.second.colors;
	for (auto &entry : _softSmokeBlends) delete entry.second.colors;
	// Tell the upscaling worker to stop (under its lock, so it can't miss the flag), wake it,
	// and wait for it to end.
	{
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		_upscaleStop = true;
	}
	_upscaleWake.notify_all();
	if (_upscaleThread.joinable()) _upscaleThread.join();
	// Only then free what it made.
	for (auto &entry : _upscaledSprites) delete entry.second.surface;
}

/**
 * Sets the battle to render, and forgets everything about the previous one: voxel models,
 * doors, particles, shots, flares, marks on the walls, units being watched and so on. Then
 * builds all the voxel models the new battle can need (see preloadModels), finds the crafts'
 * mirror symmetry (see findMirrors) and the UFOs (see findUfos).
 * @param save The battle to show.
 */
void FirstPersonView::setSavedBattleGame(SavedBattleGame *save)
{
	// Clear every piece of state that belongs to a battle.
	_save = save;
	for (auto &entry : _softSmoke) delete entry.second.colors;
	_softSmoke.clear();
	for (auto &entry : _stippleSmoke) delete entry.second.colors;
	_stippleSmoke.clear();
	for (auto &entry : _softSmokeBlends) delete entry.second.colors;
	_softSmokeBlends.clear();
	_models.clear();
	_ghostModels.clear();
	_frames.clear();
	_solidMasks.clear();
	_paintedFloors.clear();
	_tileMemos.clear();
	_partMemos.clear();
	_tileFlags.clear();
	_tileLights.clear();
	_wallsLastFrame[0].clear();
	_wallsLastFrame[1].clear();
	_doorAnimations.clear();
	_particles.clear();
	_seenExplosions.clear();
	_lastProjectile = nullptr;
	_weatherChosen = false;
	_groundModel = nullptr;
	_waterModel = nullptr;
	_unitGlides.clear();
	_bubbleVents.clear();
	_beams.clear();
	_ufoDoorOpening.clear();
	_flares.clear();
	_watchedUnits.clear();
	_decals.clear();
	_scorches.clear();
	_burningLastFrame.clear();
	for (auto &parts : _partsLastFrame) parts.clear();
	_groundChosen = false;
	_cameraUnit = nullptr;
	// Build the new battle's voxel models now rather than while playing (UFO parts are built a
	// little differently, so first which data sets those are).
	collectUfoDataSets();
	preloadModels();
	findMirrors();
	findUfos();
}

/**
 * Builds every voxel model the battle can show before it's needed, so nothing is built while
 * playing (turning towards new terrain, a wall blown into its rubble, a door opening): every
 * record of the map's terrain sets in each of its eight animation frames, and the sliding UFO
 * doors' cut-outs (each frame of the door sliding open, with the voxels left out where the door
 * has gone; every frame but the first, closed one). Logs how many models there are and how
 * long it took.
 */
void FirstPersonView::preloadModels()
{
	// Nothing to build without a battle, or for a view that borrows another view's models.
	if (!_save || _sharedModelsOwner)
	{
		return; // a second view (the soldier card) shares the main view's models
	}
	// Every record of every terrain set the map uses, in every animation frame.
	const Uint32 startTick = SDL_GetTicks();
	for (MapDataSet *dataSet : *_save->getMapDataSets())
	{
		for (size_t record = 0; record < dataSet->getSize(); ++record)
		{
			const MapData *mapData = dataSet->getObject(record);
			const Frames &frames = getFrames(mapData);
			for (int frame = 0; frame < 8; ++frame)
			{
				getModel(mapData, frames.sprite[frame], false);
				if (mapData->isUFODoor() && frame > 0) getModel(mapData, frames.sprite[frame], true);
			}
		}
	}
	Log(LOG_INFO) << "OpenXcom3D: " << _models.size() << " voxel models ready in " << SDL_GetTicks() - startTick << " ms";
}

/**
 * Places the camera's eye.
 * @param x East-west position, in voxels (16 per tile).
 * @param y North-south position, in voxels (16 per tile).
 * @param z Height, in voxels (24 per level).
 */
void FirstPersonView::setEye(double x, double y, double z)
{
	_eye[0] = x;
	_eye[1] = y;
	_eye[2] = z;
}

/**
 * Gets the camera's eye, in voxels (x east, y south, z up).
 * @param out Receives the three coordinates.
 */
void FirstPersonView::getEye(double out[3]) const
{
	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] = _eye[axis];
	}
}

/**
 * Looks along one of the game's unit directions, level. The game counts directions from north
 * clockwise in 45 degree steps, while the camera's yaw counts from east towards south, hence
 * the 90 degree shift (direction 2, east, is yaw 0).
 * @param unitDirection 0 = north (-y), clockwise in 45 degree steps.
 */
void FirstPersonView::lookAlong(int unitDirection)
{
	_yaw = (unitDirection * 45.0 - 90.0) * Pi / 180.0;
	_pitch = 0.0;
}

/**
 * Turns the camera. The yaw wraps around (kept within -180..180 degrees); the pitch stops at 80
 * degrees up or down, short of straight up or down where the view would flip over.
 * @param yawDegrees How far to turn sideways, in degrees; positive turns right.
 * @param pitchDegrees How far to tilt, in degrees; positive looks up.
 */
void FirstPersonView::rotate(double yawDegrees, double pitchDegrees)
{
	_yaw = std::remainder(_yaw + yawDegrees * Pi / 180.0, 2.0 * Pi);
	_pitch = std::clamp(_pitch + pitchDegrees * Pi / 180.0, -MaxPitch * Pi / 180.0, MaxPitch * Pi / 180.0);
}

/**
 * Points the camera at a point (used to look at an enemy). The pitch is limited to MaxPitch
 * degrees up or down, as in rotate. A point at the eye itself leaves the camera as it is.
 * @param target The point to look at, in voxels.
 */
void FirstPersonView::lookAt(const double target[3])
{
	double yawDegrees, pitchDegrees;
	if (anglesTo(target, yawDegrees, pitchDegrees))
	{
		_yaw = yawDegrees * Pi / 180.0;
		_pitch = pitchDegrees * Pi / 180.0;
	}
}

/**
 * The way the camera would point looking at a point from where its eye is now (for lookAt, and
 * for a view gliding round to look at something). The pitch is limited to MaxPitch degrees up
 * or down, as in rotate.
 * @param target The point, in voxels.
 * @param yawDegrees Set to the yaw (0 east, clockwise as seen from above, -180 to 180).
 * @param pitchDegrees Set to the pitch (up positive).
 * @return False for a point at the eye itself (nothing set).
 */
bool FirstPersonView::anglesTo(const double target[3], double &yawDegrees, double &pitchDegrees) const
{
	// dx, dy, dz: from the eye to the target, in voxels; flatDistance: the same along the ground.
	const double dx = target[0] - _eye[0], dy = target[1] - _eye[1], dz = target[2] - _eye[2];
	const double flatDistance = std::sqrt(dx * dx + dy * dy);
	if (flatDistance < 1e-6 && std::abs(dz) < 1e-6)
	{
		return false;
	}
	// The pitch is in true proportions, where a voxel up is only ZScale of one across, so the
	// height difference is scaled down before the angle is taken.
	yawDegrees = std::atan2(dy, dx) * 180.0 / Pi;
	pitchDegrees = std::clamp(std::atan2(dz * ZScale, flatDistance) * 180.0 / Pi, -MaxPitch, MaxPitch);
	return true;
}

/**
 * Tilts the view to one side about its own axis (a roll), as a head tilts when leaning out,
 * kept within 20 degrees either way. The ray caster and everything projected take the tilt as it
 * is; sprites stay upright in the world, so they tilt on the screen with the rest of it.
 * @param degrees The tilt, in degrees: positive tilts the head to the left (the world turns
 *                clockwise on the screen), negative to the right, 0 level.
 */
void FirstPersonView::setRoll(double degrees)
{
	_roll = std::clamp(degrees, -20.0, 20.0) * Pi / 180.0;
}

/**
 * Gets the direction the camera looks around the vertical axis.
 * @return The yaw in degrees: 0 east, 90 south (clockwise seen from above); within -180..180
 *         once the camera has been turned (lookAlong may leave it up to 225).
 */
double FirstPersonView::getYawDegrees() const
{
	return _yaw * 180.0 / Pi;
}

/**
 * Gets how far the camera looks up or down (without the recoil kick).
 * @return The pitch in degrees: positive up, negative down.
 */
double FirstPersonView::getPitchDegrees() const
{
	return _pitch * 180.0 / Pi;
}

/**
 * Sets the horizontal field of view, kept between 5 (zoomed in) and 120 degrees. The view
 * stores the tangent of half of it, which is what the ray directions are built from.
 * @param degrees The horizontal field of view, in degrees.
 */
void FirstPersonView::setFieldOfView(double degrees)
{
	_halfFovTangent = std::tan(std::clamp(degrees, 5.0, 120.0) * Pi / 360.0);
}

/**
 * Sets how far the view reaches: rays stop there, and the world fades into the haze before it.
 * @param tiles The distance in tiles (stored in voxels, 16 per tile).
 */
void FirstPersonView::setMaxDistance(int tiles)
{
	_maxDistance = (float)(tiles * Position::TileXY);
}

/**
 * Works out the camera's axes from its yaw and pitch: forward, right and up, as unit vectors
 * in true proportions (see ZScale), and forward again as a direction in the voxel grid.
 * The recoil kick tilts the view up for the moment without changing the pitch itself.
 * A lean's tilt (setRoll) then turns right and up about the forward axis; the axes without it
 * are kept too, for the sprites standing square to the view (see drawSprite).
 * x east, y south, z up is a left-handed frame, hence up = forward x right.
 */
void FirstPersonView::updateBasis()
{
	// The pitch with the recoil kick added, within the same limits as rotate (MaxPitch).
	const double viewPitch = std::clamp(_pitch + _recoilKick * Pi / 180.0, -MaxPitch * Pi / 180.0, MaxPitch * Pi / 180.0); // recoil shows, it doesn't stay
	const double cosYaw = std::cos(_yaw), sinYaw = std::sin(_yaw), cosPitch = std::cos(viewPitch), sinPitch = std::sin(viewPitch);
	// Forward along the yaw, tilted by the pitch; right level (the tilt comes below); up from the two.
	_forward[0] = cosYaw * cosPitch; _forward[1] = sinYaw * cosPitch; _forward[2] = sinPitch;
	_right[0] = -sinYaw; _right[1] = cosYaw; _right[2] = 0.0;
	_up[0] = -_forward[2] * _right[1];
	_up[1] = _forward[2] * _right[0];
	_up[2] = _forward[0] * _right[1] - _forward[1] * _right[0];
	// The tilt: right rises and up leans towards the left by the roll (both stay square to
	// forward and to each other).
	const double cosRoll = std::cos(_roll), sinRoll = std::sin(_roll);
	for (int axis = 0; axis < 3; ++axis)
	{
		_levelRight[axis] = _right[axis];
		_levelUp[axis] = _up[axis];
		_right[axis] = cosRoll * _levelRight[axis] + sinRoll * _levelUp[axis];
		_up[axis] = cosRoll * _levelUp[axis] - sinRoll * _levelRight[axis];
	}
	// Forward in the voxel grid: a voxel up is shorter than one across, so the same height takes
	// more voxels (divide by ZScale), then make it a unit vector again.
	const double forwardZInVoxels = _forward[2] / ZScale;
	const double lengthInVoxels = std::sqrt(_forward[0] * _forward[0] + _forward[1] * _forward[1] + forwardZInVoxels * forwardZInVoxels);
	_forwardInVoxels[0] = _forward[0] / lengthInVoxels;
	_forwardInVoxels[1] = _forward[1] / lengthInVoxels;
	_forwardInVoxels[2] = forwardZInVoxels / lengthInVoxels;
}

/**
 * Gets the direction of the ray through a pixel of the view (through the pixel's centre), in
 * the voxel grid, as a unit vector. Used to aim (what's under the crosshair), to start our own
 * shots from the weapon's muzzle on the screen, and to lay shadows on the ground under units.
 * The pixels are square: the vertical field of view follows from the horizontal one and the
 * view's shape.
 * @param pixelX The pixel's column, from the left.
 * @param pixelY The pixel's row, from the top.
 * @param out Receives the direction (x east, y south, z up).
 */
void FirstPersonView::getRayDirection(int pixelX, int pixelY, double out[3])
{
	updateBasis();
	rayDirectionThrough(pixelX, pixelY, out);
}

/**
 * The direction of the ray through a pixel, as getRayDirection, with the camera's axes as they
 * are: for the drawing of a frame, which works them out first (see draw), and may want a ray for
 * every pixel of a patch of the view (a unit's shadow on the ground, see drawGroundShade).
 * @param pixelX The pixel's column, from the left.
 * @param pixelY The pixel's row, from the top.
 * @param out Receives the direction (x east, y south, z up).
 */
void FirstPersonView::rayDirectionThrough(int pixelX, int pixelY, double out[3]) const
{
	// Where the pixel lies on an image plane one unit in front of the eye: planeX to the right,
	// from -tan(half the field of view) to +tan; planeY up, over the same range shrunk by the
	// view's height over its width.
	const int viewWidth = getWidth(), viewHeight = getHeight();
	const double planeX = ((pixelX + 0.5) * 2.0 / viewWidth - 1.0) * _halfFovTangent;
	const double planeY = (1.0 - (pixelY + 0.5) * 2.0 / viewHeight) * _halfFovTangent * viewHeight / viewWidth;
	double length = 0.0;
	// The direction in true proportions: forward, plus that much right and up.
	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] = _forward[axis] + planeX * _right[axis] + planeY * _up[axis];
	}
	out[2] /= ZScale; // into the voxel grid
	// Make it a unit vector.
	for (int axis = 0; axis < 3; ++axis)
	{
		length += out[axis] * out[axis];
	}
	length = std::sqrt(length);
	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] /= length;
	}
}

/**
 * Gets the game's shade where the camera is, from this frame's snapshot of the tiles' light
 * (blended between tiles with smooth lighting on, as the view draws it). The first-person mode
 * shades the weapon in hand by it.
 * @return The shade: 0 brightest .. 15 darkest.
 */
int FirstPersonView::getEyeShade() const
{
	// Before the first frame there's no snapshot of the tiles to read the light from.
	if (!_save || (int)_tileLights.size() != _save->getMapSizeXYZ())
	{
		return 0;
	}
	return (int)lightAt((float)_eye[0], (float)_eye[1], (float)_eye[2]);
}

/**
 * Works out where a sound from a tile is heard, with this camera as the listener: the stereo
 * angle and how far away it sounds, packed into one number for Sound (see
 * Sound::packDistance). The angle is 0 straight ahead, 90 to the right, 180 behind, 270 to the
 * left (as Mix_SetPosition takes it).
 * @param tile The tile the sound comes from.
 * @return The angle with the distance packed in, or 0 for a sound right here.
 */
int FirstPersonView::soundAngle(const Position &tile) const
{
	// dx, dy, dz: from the eye to the middle of the tile, in true proportions (voxels across).
	const double dx = tile.x * 16.0 + 8.0 - _eye[0], dy = tile.y * 16.0 + 8.0 - _eye[1];
	const double dz = (tile.z * 24.0 + 12.0 - _eye[2]) * ZScale; // a level is 24 voxels up, a voxel up sqrt(2/3) of one across
	// Within 12 voxels: no direction at all.
	if (dx * dx + dy * dy < 12.0 * 12.0 && std::abs(dz) < 12.0)
	{
		return 0; // right here: both ears, full volume
	}
	// The direction of the sound relative to the way the camera faces, 0..360 degrees clockwise.
	double angle = (std::atan2(dy, dx) - _yaw) * 180.0 / Pi;
	angle = std::fmod(angle + 720.0, 360.0);
	// Quieter with distance: full within two tiles, fading over the next twenty-odd; and a
	// little quieter behind you (stereo alone can't tell front from back), up to 30 more for a
	// sound straight behind, nothing beyond 70 degrees from behind. The distance is SDL_mixer's
	// 0 (loud) .. 255 (silent), capped at 220 so nothing goes quite silent.
	const double distanceInTiles = std::sqrt(dx * dx + dy * dy + dz * dz) / 16.0;
	double mixerDistance = std::max(0.0, distanceInTiles - 2.0) * 9.0;
	const double behind = 1.0 - std::abs(angle - 180.0) / 70.0;
	if (behind > 0.0) mixerDistance += 30.0 * behind;
	return Sound::packDistance((int)angle, (int)std::min(220.0, mixerDistance));
}

/**
 * Renders one frame of the view. Terrain, the map border and the sky are drawn by casting a
 * ray per pixel (spread over a few threads); then everything else is drawn over that picture,
 * hidden where the depth buffer says the world is nearer: marks on the walls, items, units,
 * fire (and the old solid smoke), shots and explosions, flares, beams, the see-through smoke
 * (over all that, farthest first), particles and glows. Then the effects on
 * the screen itself: the colour wash, flames when the soldier burns, the hurt arc and blood, and
 * last the icons over the downed units and the enemy markers, over everything. A helmet
 * camera's feed now and then shows a bad signal (see updateSignalGlitches): blocky patches and
 * green squares over the picture, or the last picture held a moment.
 * Without a battle or a camera unit, the surface is just cleared.
 */
void FirstPersonView::draw()
{
	// Nothing to look through yet: a blank surface.
	if (!_save || !_cameraUnit)
	{
		Surface::draw();
		return;
	}
	_redraw = false;
	// A camera's feed with a bad signal may hold its last picture a moment (see updateSignalGlitches).
	if (_signalGlitches && updateSignalGlitches())
	{
		return;
	}
	// Upscaled sprites used from now on belong to this frame (see upscaled).
	_upscaleFrameStart = _upscaleClock + 1;

	// The camera for this frame: shake and recoil, then its axes; a camera's feed's static spells.
	updateCameraEffects();
	if (_staticSpells) updateStaticSpell();

	updateBasis();
	// The flashlight sits on the right shoulder, a little below the eye (voxels); the dynamic
	// one just under the eye, on the weapon, so its spot falls on the crosshair.
	for (int axis = 0; axis < 3; ++axis)
	{
		_flashlightPos[axis] = _dynamicFlashlight ? (float)(_eye[axis] - _up[axis] * 1.5 + _forward[axis] * 1.0)
			: (float)(_eye[axis] + _right[axis] * 4.0 - _up[axis] * 4.0 + _forward[axis] * 2.0);
	}
	// What the world looks like this frame: the tiles, the ground beyond the map (chosen once),
	// the palette colours for the time of day, and the particles moved on.
	snapshotTiles();
	if (!_groundChosen)
	{
		chooseGround();
	}
	updateColors();
	updateParticles();

	// Draw straight into the surface's pixels.
	lock();
	// The world: terrain, border and sky, ray by ray (fills the depth buffer).
	castAllRays();
	// What's in the world: first the units are checked for hits and kills (for their flinches
	// and the hitmarker), then everything is drawn over the terrain as sprites, dots and lines.
	// (No unit is on the screen yet, for the see-through smoke.)
	std::fill(_unitVeil.begin(), _unitVeil.end(), (Uint8)255);
	std::fill(&_pointerUnits[0][0], &_pointerUnits[0][0] + sizeof(_pointerUnits) / sizeof(_pointerUnits[0][0]), nullptr);
	std::fill(&_pointerBodies[0][0], &_pointerBodies[0][0] + sizeof(_pointerBodies) / sizeof(_pointerBodies[0][0]), nullptr);
	watchUnits();
	drawDecals();
	drawFloorItems();
	drawEdgeDots();
	drawUnits();
	drawFire();
	drawProjectilesAndExplosions();
	drawFlares();
	drawBeams();
	drawPellets();
	drawTrajectory(false);
	drawSeeThroughSmoke();
	findUnitAtPointer();
	drawParticles();
	drawLightHalos();
	// Effects on the finished picture (a camera's feed: its bad signal last). Under night vision
	// the aimed path is light in the goggles: drawn again over the green, white-green.
	applyScreenWash();
	if (_nightVision) drawTrajectory(true);
	if (_signalGlitches) applySignalGlitches();
	drawBurning();
	drawHurt();
	// The icons over the downed units, the enemy markers and the units' facing rings, over everything.
	drawDownedIcons();
	drawMarkers();
	drawSprayMarks();
	drawFacing();
	unlock();
}

/**
 * Moves the camera's effects on by one frame: the shake jitters the eye (smooth wobbles that
 * die away), the recoil kick settles back, and the last muzzle flash's light fades out over
 * 90 ms (if the muzzle flash option is on).
 */
void FirstPersonView::updateCameraEffects()
{
	// Seconds since the last frame, at most a tenth of a second (a long pause doesn't make the
	// shake vanish in one jump), and nothing on the first frame.
	const Uint32 now = SDL_GetTicks();
	const float elapsedSeconds = _shakeLastTick ? std::min<Uint32>(100, now - _shakeLastTick) / 1000.0f : 0.0f;
	_shakeLastTick = now;
	// Camera shake: the eye jitters for this frame only, dying away (halving about every tenth
	// of a second).
	if (_shake > 0.02f)
	{
		// Smooth shaking (a few wobbles a second, from sines of the time), not a new random jolt
		// every frame; less sideways than up and down.
		const float seconds = now / 1000.0f;
		_eye[0] += _shake * 0.6f * std::sin(seconds * 41.0f) * std::cos(seconds * 13.0f);
		_eye[1] += _shake * 0.6f * std::sin(seconds * 37.0f + 1.3f);
		_eye[2] += _shake * std::sin(seconds * 47.0f + 2.1f) * std::cos(seconds * 11.0f);
		_shake *= std::exp(-elapsedSeconds * 7.0f);
	}
	else
	{
		_shake = 0.0f;
	}
	// The recoil kick eases back down, a little faster than the shake.
	_recoilKick *= std::exp(-elapsedSeconds * 9.0f);
	if (_recoilKick < 0.02f) _recoilKick = 0.0f;
	// The muzzle flash lights the surroundings at full strength, fading to nothing over 90 ms.
	const Uint32 sinceFlash = SDL_GetTicks() - _muzzleFlashTick;
	_muzzleFlashLight = Options::fpsMuzzleFlash && _muzzleFlashTick && sinceFlash < 90 ? 1.0f - sinceFlash / 90.0f : 0.0f;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
