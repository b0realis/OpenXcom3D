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
#include "../Battlescape/Map.h"
#include "../Savegame/Tile.h"
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <SDL.h>
#ifdef __ANDROID__
#include <fstream>
#include <sstream>
#include <thread>
#include <android/log.h>
#endif
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Sound.h"
#include "../Engine/Surface.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Camera.h"
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
	// it shines (for the bloom), how much see-through smoke may still veil a unit seen there,
	// and whether a unit's or an item's picture shows there.
	_depthBuffer.resize(width * height);
	_glowMask.resize(width * height);
	_unitVeil.resize(width * height);
	_smokeVeil.resize(width * height);
	_ownColours.resize(width * height);

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
	delete _isoPartBuffer;
	for (auto &entry : _softSmoke) delete entry.second.colors;
	for (auto &entry : _stippleSmoke) delete entry.second.colors;
	for (auto &entry : _softSmokeBlends) delete entry.second.colors;
	clearArtOverlays();
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
	gpuWarmUp();
	gpuForgetModels();
	_models.clear();
	_sameArtModels.clear();
	_ghostModels.clear();
	_voxelUnits->forBattle(save);
	_collapseStarts.clear();
	_facingsWell.clear();
	_leafyModels.clear();
	_partKinds.clear();
	_liftRecords.clear();
	_flameParts.clear();
	clearArtOverlays();
	_overlaySpots.clear();
	_overlaySpotsAge = 0;
	_openedModels.clear();
	_openDoorModels.clear();
	_frames.clear();
	_turnedParts.clear();
	_solidMasks.clear();
	_paintedFloors.clear();
	_anyGlass = false;
	_wallPaints.clear();
	_wallPaintOf.clear();
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
	_skyChosen = false;
	_groundModel = nullptr;
	_waterModel = nullptr;
	_unitGlides.clear();
	_bubbleVents.clear();
	_beams.clear();
	_ufoDoorOpening.clear();
	_flares.clear();
	_watchedUnits.clear();
	_obliterated.clear();
	_bloodColors.clear();
	_bloodUnits.clear();
	_bloodUnitsCount = 0;
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
	findLifts();
	// (OPENXCOM3D_ANIMATION_CHECK=file: a developer's list of every animated record and how it shows; see checkAnimatedParts.)
	static const char *const animationCheck = getenv("OPENXCOM3D_ANIMATION_CHECK");
	if (animationCheck && save && !_sharedModelsOwner)
	{
		checkAnimatedParts(animationCheck);
	}
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
			artOverlay(mapData); // (the art of an animated part its model can't show, if any)
		}
	}
	Log(LOG_INFO) << "OpenXcom3D: " << _models.size() << " voxel models ready in " << SDL_GetTicks() - startTick << " ms";
	if (!_flameParts.empty())
	{
		Log(LOG_INFO) << "OpenXcom3D: " << _flameParts.size() << " kinds of flame on the map are drawn as sprites";
	}
	if (_anyArtOverlay)
	{
		int kinds = 0;
		for (const auto &entry : _artOverlays) kinds += entry.second.frames[0] != nullptr;
		Log(LOG_INFO) << "OpenXcom3D: " << kinds << " kinds of animated part have art their shape can't show: drawn as sprites";
	}
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
	std::copy(_eye, _eye + 3, _steadyEye); // (until the next frame's shake, see updateCameraEffects)
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
 * Gets the camera's eye as placed for this frame, before the shake jitters it (see
 * updateCameraEffects), in voxels: the side units are drawn from is worked out from it, so a
 * shake doesn't turn them (GitHub issue #112).
 * @param out Receives the three coordinates.
 */
void FirstPersonView::getSteadyEye(double out[3]) const
{
	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] = _steadyEye[axis];
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
 * stores the tangent of half of it, which is what the ray directions are built from; and how
 * many times the zoom narrowed it, for what keeps its size on the screen as if unzoomed (the
 * bullet holes' cap right against them, see drawDecals).
 * @param degrees The horizontal field of view, in degrees.
 * @param zoom How many times the zoom narrows it (1: not zoomed).
 */
void FirstPersonView::setFieldOfView(double degrees, double zoom)
{
	_halfFovTangent = std::tan(std::clamp(degrees, 5.0, 120.0) * Pi / 360.0);
	_zoom = std::max(1.0, zoom);
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
 * 3DIso (research): the isometric map's camera. The map draws a voxel at screen
 * (offset + 16 + x - y, offset + 24 + (x + y) / 2 - z) (the art's own anchor, see buildModel), its centre on the
 * pixel's centre half a pixel on: the view of the voxel grid down its diagonal (1, 1, 1), which in true
 * proportions is yaw -135 and pitch -30 (asin(tan(atan(1/2)))), sqrt(2) pixels to a voxel across. A camera so
 * far back that its rays are parallel to well under a pixel draws that with the perspective code as it is; the
 * rays start where they enter the map under the view level's top (see renderRows).
 */
void FirstPersonView::isoPivot(double mapOffsetX, double mapOffsetY, int viewLevel, double pivot[3]) const
{
	const double z = viewLevel * (double)Position::TileZ;
	const double across = isoMapWidth() / 2.0 + _isoShift[0] - mapOffsetX - 16.0, down = isoMapHeight() / 2.0 + _isoShift[1] - mapOffsetY - 24.0 + z;
	pivot[0] = down + across / 2.0;
	pivot[1] = down - across / 2.0;
	pivot[2] = z;
}

void FirstPersonView::setFlightCamera(const double eye[3], double yawDegrees, double pitchDegrees, double halfFovTangent, float cutTop, const BattleUnit *lookThrough, float shown3D)
{
	_iso = false;
	_flight = true;
	_isoTurned = false;
	_isoTop = cutTop;
	_isoCutSoftness = 0.0f;
	_isoLowLevel = 0;
	_eye[0] = _steadyEye[0] = eye[0];
	_eye[1] = _steadyEye[1] = eye[1];
	_eye[2] = _steadyEye[2] = eye[2];
	_yaw = yawDegrees * Pi / 180.0;
	_pitch = pitchDegrees * Pi / 180.0;
	_roll = 0.0;
	_halfFovTangent = halfFovTangent;
	_zoom = 1.0;
	_cameraUnit = lookThrough;
	_flightShown3D = shown3D;
	updateBasis();
}

void FirstPersonView::keepPicture()
{
	_keptPicture.resize((size_t)getWidth() * getHeight());
	lock();
	for (int y = 0; y < getHeight(); ++y) std::copy(getRaw(0, y), getRaw(0, y) + getWidth(), _keptPicture.begin() + (size_t)y * getWidth());
	unlock();
}

void FirstPersonView::setIsoCamera(double mapOffsetX, double mapOffsetY, int viewLevel, int lowLevel, int highLevel)
{
	// (voxels: parallel to 0.1 px across a map's depth; the float depth buffer keeps 1/16 voxel. Tilted flatter than the map's
	// slant, a slight perspective, as a camera looking across the map has it: the eye nearer the flatter it is, IsoNearBack at
	// the flattest (the change in 1 / distance even), the width seen at the middle the same.)
	const double flatter = std::clamp(-_isoTilt / 20.0, 0.0, 1.0);
	const double FarBack = flatter > 0.0 ? 1.0 / (1.0 / IsoFarBack + flatter * (1.0 / IsoNearBack - 1.0 / IsoFarBack)) : IsoFarBack;
	_isoBack = FarBack;
	_iso = true;
	_flight = false;
	_isoTurned = false;
	_cameraUnit = nullptr;
	_isoViewLevel = highLevel;
	_isoLowLevel = lowLevel;
	_isoTop = (float)((highLevel + 1) * Position::TileZ);
	_isoOffset[0] = mapOffsetX;
	_isoKeys.resize((size_t)getWidth() * getHeight());
	_isoMarkDepth.resize((size_t)getWidth() * getHeight());
	_isoOffset[1] = mapOffsetY;
	_yaw = -0.75 * Pi + _isoTurn * Pi / 180.0; // (turned: see setIsoTurn)
	_pitch = std::clamp(-Pi / 6.0 - _isoTilt * Pi / 180.0, -80.0 * Pi / 180.0, -10.0 * Pi / 180.0); // (tilted: see setIsoTilt)
	_roll = 0.0;
	updateBasis();
	// The point the view's middle shows on the view level's floor, in voxels.
	// (the map's canvas, not the view: drawn bigger, see setIsoScale, it shows the same middle)
	const double width = getWidth(), mapWidth = isoMapWidth(), mapHeight = isoMapHeight();
	const double z = viewLevel * (double)Position::TileZ;
	const double across = mapWidth / 2.0 + _isoShift[0] - mapOffsetX - 16.0, down = mapHeight / 2.0 + _isoShift[1] - mapOffsetY - 24.0 + z; // (the art: a tile's north corner at its sprite's (16, 24), on a pixel edge; scrolled smoothly, see setIsoShift)
	const double middle[3] = { down + across / 2.0, down - across / 2.0, z };
	_isoMiddle[0] = middle[0];
	_isoMiddle[1] = middle[1];
	std::copy(middle, middle + 3, _isoCentre);
	// The eye that far back from it along the view (true proportions), back in voxels; a sqrt(2) pixels a voxel there.
	for (int axis = 0; axis < 3; ++axis)
	{
		const double trueMiddle = middle[axis] * (axis == 2 ? ZScale : 1.0);
		const double trueEye = trueMiddle - _forward[axis] * FarBack;
		_eye[axis] = _steadyEye[axis] = axis == 2 ? trueEye / ZScale : trueEye;
	}
	_halfFovTangent = (width / 2.0) / (std::sqrt(2.0) * FarBack * _isoZoomTotal);
	_zoom = 1.0;
	_maxDistance = (float)(FarBack + 8192.0);
	if (std::abs(_isoTurn) > 1e-6 || std::abs(_isoTilt) > 1e-6)
	{
		// Turned or tilted: in 3D, as the flight draws (the rays from the same far eye, the levels above the view level cut
		// away), the world past the map black.
		_iso = false;
		_flight = true;
		_isoTurned = true;
	}
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
	// VR: a head's (or an eye's) own axes, as they are (see setHeadAxes): no pitch limit, no recoil kick. The level ones,
	// for what stands square to the view, from its yaw: right along the ground, and up square to that and forward.
	if (_headAxes)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			_forward[axis] = _headForward[axis];
			_right[axis] = _headRight[axis];
			_up[axis] = _headUp[axis];
		}
		_levelRight[0] = -std::sin(_yaw); _levelRight[1] = std::cos(_yaw); _levelRight[2] = 0.0;
		_levelUp[0] = -_forward[2] * _levelRight[1];
		_levelUp[1] = _forward[2] * _levelRight[0];
		_levelUp[2] = _forward[0] * _levelRight[1] - _forward[1] * _levelRight[0];
		const double levelUpLength = std::sqrt(_levelUp[0] * _levelUp[0] + _levelUp[1] * _levelUp[1] + _levelUp[2] * _levelUp[2]);
		for (double &component : _levelUp) component /= std::max(1e-9, levelUpLength);
		const double forwardZInVoxels = _forward[2] / ZScale;
		const double lengthInVoxels = std::sqrt(_forward[0] * _forward[0] + _forward[1] * _forward[1] + forwardZInVoxels * forwardZInVoxels);
		_forwardInVoxels[0] = _forward[0] / lengthInVoxels;
		_forwardInVoxels[1] = _forward[1] / lengthInVoxels;
		_forwardInVoxels[2] = forwardZInVoxels / lengthInVoxels;
		return;
	}
	// The pitch with the recoil kick added, within the same limits as rotate (MaxPitch).
	const double viewPitch = std::clamp(_pitch + (Options::fpsRecoil ? _recoilKick : 0.0f) * Pi / 180.0, -MaxPitch * Pi / 180.0, MaxPitch * Pi / 180.0); // recoil shows, it doesn't stay (none with the setting "Recoil" off; the gun in hand still kicks)
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
 * @param pixelX The pixel's column, from the left (a fraction moves the ray off the pixel's centre: see FirstPersonMode::aimAt).
 * @param pixelY The pixel's row, from the top.
 * @param out Receives the direction (x east, y south, z up).
 */
void FirstPersonView::getRayDirection(double pixelX, double pixelY, double out[3])
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
void FirstPersonView::rayDirectionThrough(double pixelX, double pixelY, double out[3]) const
{
	// Where the pixel lies on an image plane one unit in front of the eye: planeX to the right,
	// from -tan(half the field of view) to +tan; planeY up, over the same range shrunk by the
	// view's height over its width.
	// (In VR an eye's picture is off centre: its view axis meets it at (axisX, axisY), and a unit of the plane is
	// focalPixels pixels.)
	const int viewWidth = getWidth(), viewHeight = getHeight();
	const double planeX = _eyePass ? (pixelX + 0.5 - _eyeAxisX) / _eyeFocal : ((pixelX + 0.5) * 2.0 / viewWidth - 1.0) * _halfFovTangent;
	const double planeY = _eyePass ? (_eyeAxisY - (pixelY + 0.5)) / _eyeFocal : (1.0 - (pixelY + 0.5) * 2.0 / viewHeight) * _halfFovTangent * viewHeight / viewWidth;
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
 * The game's shade where the camera is, as getEyeShade has it but not cut to a whole shade
 * (see FirstPersonMode::updateWeaponInHand, which steadies it).
 */
float FirstPersonView::getEyeLight() const
{
	if (!_save || (int)_tileLights.size() != _save->getMapSizeXYZ())
	{
		return 0.0f;
	}
	return lightAt((float)_eye[0], (float)_eye[1], (float)_eye[2]);
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
	if (!_save || (!_cameraUnit && !_iso && !_flight))
	{
		Surface::draw();
		return;
	}
	_redraw = false;
	// (OPENXCOM3D_RAY_TIME: the log gets the main view's whole drawing time too, averaged over 120 frames.)
	static const bool drawTime = getenv("OPENXCOM3D_RAY_TIME") != nullptr;
	struct DrawTimer
	{
		const FirstPersonView *view;
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		~DrawTimer()
		{
			if (!view) return;
			static double sum = 0.0;
			static int frames = 0;
			sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			if (++frames == 120)
			{
				Log(LOG_INFO) << "OpenXcom3D: the " << view->getWidth() << "x" << view->getHeight() << " view's drawing takes " << sum / frames << " ms in all";
				sum = 0.0;
				frames = 0;
			}
		}
	} drawTimer { drawTime && getWidth() >= 320 ? this : nullptr };
	// A camera's feed with a bad signal may hold its last picture a moment (see updateSignalGlitches).
	if (_signalGlitches && updateSignalGlitches())
	{
		return;
	}
#ifdef __ANDROID__
	// (The frame's start, for the Android build's frame probe.)
	const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
	// (Setting "Render scale" on Auto goes by how long the frame took: see noteDrawTime.)
	struct ScaleTimer
	{
		FirstPersonView *view;
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		~ScaleTimer() { if (!view->_iso && !view->_isoTurned) view->noteDrawTime(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()); }
	} scaleTimer { this };
	_gpuCastRays = false;
	// The frame's shared work, then its one picture (VR's renderEyes draws two of them from the one shared part): all
	// of it but the units' facing rings, which close it here (and each of renderEyes's), over everything.
	// (OXC3D_ISO3D_TIME: 3DIso's frame in parts, averaged over 120 frames, in the log)
	static const bool isoTime = getenv("OXC3D_ISO3D_TIME") && *getenv("OXC3D_ISO3D_TIME");
	static double isoSums[8] = {};
	static int isoFrames = 0;
	auto isoClock = std::chrono::steady_clock::now();
	auto isoLap = [&](int part) {
		if (!isoTime || !(_iso || _isoTurned)) return;
		const auto now = std::chrono::steady_clock::now();
		isoSums[part] += std::chrono::duration<double, std::milli>(now - isoClock).count();
		isoClock = now;
	};
	beginFrame();
	isoLap(0);
	if (_iso) prepareIsoSmoothArt();
	if (_iso) prepareIsoTileArt();
	isoLap(1);
	if (_iso && (_isoExtras & IsoExtraTrueColour))
	{
		// (extra "true colour": the painter keeps its pixels' full colours, see renderRows; the rest shows the palette's)
		_isoFull.assign((size_t)getWidth() * getHeight(), 0u);
		_isoPainted.assign((size_t)getWidth() * getHeight(), 0);
	}
	drawPass();
	isoLap(2);
	if (!_iso || (_isoExtras & IsoExtraFacing)) drawFacing();
	if ((_flight || _iso) && _flightShown3D < 1.0f && _keptPicture.size() == (size_t)getWidth() * getHeight())
	{
		// The flight's dissolve: the kept picture (the map's) on the share of pixels the 3D one doesn't have yet, by a 4x4
		// ordered dither.
		for (int y = 0; y < getHeight(); ++y)
		{
			Uint8 *row = getRaw(0, y);
			for (int x = 0; x < getWidth(); ++x)
				if (dither(x, y) >= _flightShown3D) row[x] = _keptPicture[(size_t)y * getWidth() + x];
		}
	}
	if (_iso && _isoOverlay && _isoOverlay->getWidth() == isoMapWidth() && _isoOverlay->getHeight() == isoMapHeight())
	{
		// 3DIso: the map's overlays over the world, as the map draws them over its own (see Map::setOverlayTarget); drawn
		// bigger (see setIsoScale), each view pixel takes its map pixel's.
		SDL_Surface *overlay = _isoOverlay->getSurface();
		const int mapWidth = isoMapWidth(), mapHeight = isoMapHeight();
		std::vector<int> mapColumn(getWidth());
		for (int x = 0; x < getWidth(); ++x) { const int column = (int)std::floor(isoMapX(x)); mapColumn[x] = column >= 0 && column < mapWidth ? column : -1; }
		const std::vector<Uint32> &keys = _map->overlayKeys();
		const bool keyed = keys.size() == (size_t)mapWidth * mapHeight;
		const bool noted = keyed && _isoMapSpritesNow && !(_isoExtras & IsoExtraSmoke); // (the overlay's order kept for the smoke drawn after it)
		if (noted) _isoOverlayKeys.assign((size_t)getWidth() * getHeight(), 0u);
		for (int y = 0; y < getHeight(); ++y)
		{
			const int row = (int)std::floor(isoMapY(y));
			if (row < 0 || row >= mapHeight) continue;
			const Uint8 *from = (const Uint8*)overlay->pixels + row * overlay->pitch;
			Uint8 *to = getRaw(0, y);
			for (int x = 0; x < getWidth(); ++x)
			{
				const int column = mapColumn[x];
				if (column < 0 || !from[column]) continue;
				const size_t i = (size_t)y * getWidth() + x, mapIndex = (size_t)row * mapWidth + column;
				if (!keyed || keys[mapIndex] > _isoKeys[i])
				{
					to[x] = from[column]; // (only where the map draws it last)
					if (noted) _isoOverlayKeys[i] = keys[mapIndex];
				}
			}
		}
	}
	if (_iso && _isoMapSpritesNow && !(_isoExtras & IsoExtraSmoke) && _isoOverlay) drawIsoMapSmoke();
	isoLap(3); // (the overlays laid in, the dissolve)
	if (_isoTurned) drawIsoTurnedMarks();
	if (_iso) drawIsoEffects();
	isoLap(4);
	if (_iso && (_isoExtras & IsoExtraLayer)) drawIsoLayer();
	isoLap(5);
	if ((_iso || _isoTurned) && _map)
	{
		// (3DIso: where nothing is drawn, the map's background, as the map clears its picture: black, or TFTD's dark blue)
		const Uint8 background = (Uint8)_map->backgroundColor();
		if (background)
			for (int y = 0; y < getHeight(); ++y)
			{
				Uint8 *row = getRaw(0, y);
				for (int x = 0; x < getWidth(); ++x)
					if (!row[x]) row[x] = background;
			}
	}
	if (_iso)
	{
		// 3DIso's extras over everything, as in 3D: the icons over the downed, then the markers.
		if (_isoExtras & IsoExtraDowned) drawDownedIcons();
		if (_isoExtras & IsoExtraMarkers) drawMarkers();
	}
	if (_iso && (_isoExtras & IsoExtraTrueColour) && _isoFull.size() == (size_t)getWidth() * getHeight())
	{
		// (a pixel something was drawn over since the painter, a unit, the map's overlays, an effect, a marker: the palette's colour)
		for (int y = 0; y < getHeight(); ++y)
		{
			const Uint8 *row = getRaw(0, y);
			for (int x = 0; x < getWidth(); ++x)
			{
				const size_t at = (size_t)y * getWidth() + x;
				if (_isoFull[at] && row[x] != _isoPainted[at]) _isoFull[at] = 0;
			}
		}
	}
	isoLap(6); // (the background, the full colour's check, the extras' icons)
	if (isoTime && (_iso || _isoTurned) && ++isoFrames == 120)
	{
		static const char *const names[] = { "begin", "smoothArt", "rays+units", "overlays", "effects", "marks", "fills" };
		std::ostringstream line;
		for (int part = 0; part < 7; ++part) { line << " " << names[part] << " " << std::lround(isoSums[part] / isoFrames * 100.0) / 100.0; isoSums[part] = 0.0; }
		isoFrames = 0;
		Log(LOG_INFO) << "OpenXcom3D: 3DIso " << getWidth() << "x" << getHeight() << " ms:" << line.str();
	}
	unlock();
	if (_gpuPresenting) gpuPresent(); // (the frame put together on the card: see FirstPersonViewGpuEffects.cpp)
#ifdef __ANDROID__
	reportFrameTime(drawStart);
#endif
}

bool FirstPersonView::isoTileAt(double viewX, double viewY, int level, int &tileX, int &tileY) const
{
	double direction[3];
	rayDirectionThrough(viewX, viewY, direction);
	const double floor = level * 24.0;
	if (direction[2] > -1e-9) return false;
	const double along = (floor - _eye[2]) / direction[2];
	tileX = (int)std::floor((_eye[0] + direction[0] * along) / 16.0);
	tileY = (int)std::floor((_eye[1] + direction[1] * along) / 16.0);
	return tileX >= 0 && tileY >= 0 && tileX < _save->getMapSizeX() && tileY < _save->getMapSizeY();
}

/**
 * 3DIso turned (see setIsoTurn): what the map draws on its own side's picture and the turned one can't take from it: the
 * cursor, as a box round its tile on the view level (the map's red); the extras' markers and the downed icons.
 */
void FirstPersonView::drawIsoTurnedMarks()
{
	drawIsoLayer();
	if (_isoExtras & IsoExtraDowned) drawDownedIcons();
	if (_isoExtras & IsoExtraMarkers) drawMarkers();
}

void FirstPersonView::prepareIsoSmoothArt()
{
	// (setting "Smooth sprites", as the units have them: once a map pixel is UpscaleFrom view pixels or more. The smooth frames
	// show once every frame in sight has one, its other animation frames too, and then dissolve in over IsoSmoothFade (as they
	// dissolve out under UpscaleFrom): never some tiles smooth beside others not yet, nor a tile turning smooth on its own.
	// A frame that isn't made a while after the rest (the upscaler's queue) doesn't hold them back.)
	_isoSmoothArt.clear();
	_isoSmoothFactor = 0;
	const Uint32 now = SDL_GetTicks();
	const float step = _isoSmoothTick ? std::min(1.0f, (float)(now - _isoSmoothTick) / IsoSmoothFade) : 1.0f;
	_isoSmoothTick = now;
	const bool wanted = Options::fpsSpriteUpscale > 0 && _isoZoomTotal >= UpscaleFrom && _isoVoxelShare < 1.0f;
	if (!wanted)
	{
		_isoSmoothWaitSince = 0;
		_isoSmoothShare = std::max(0.0f, _isoSmoothShare - step);
		if (_isoSmoothShare <= 0.0f || _isoVoxelShare >= 1.0f) { _isoSmoothShare = 0.0f; return; }
	}
	else if (!_isoSmoothWaitSince) _isoSmoothWaitSince = now;
	if (Options::fpsSpriteUpscale <= 0) { _isoSmoothShare = 0.0f; return; } // (switched off: at once, nothing more asked of the upscaler)
	const int factor = spriteUpscaleFactor();
	// (made anew at another size, level of "Smooth sprites" or palette: shown again once all are)
	const Uint64 shareKey = (Uint64)factor | (Uint64)Options::fpsSpriteUpscale << 8 | (Uint64)_colors.paletteHash << 16;
	if (shareKey != _isoSmoothShareKey) { _isoSmoothShare = 0.0f; _isoSmoothShareKey = shareKey; _isoSmoothWaitSince = now; }
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	const int offsetX = (int)_isoOffset[0], offsetY = (int)_isoOffset[1];
	// (the map's pixels the view shows, and a frame's reach round its tile's corner: 32 across, 40 + 24 lifted down)
	// (half a view more on each side, made ahead of the scrolling: no art turning smooth as it comes in)
	const double marginX = getWidth() / 2.0 / _isoZoomTotal, marginY = getHeight() / 2.0 / _isoZoomTotal;
	const double viewLeft = isoMapX(-1), viewRight = isoMapX(getWidth()), viewTop = isoMapY(-1), viewBottom = isoMapY(getHeight());
	const double left = viewLeft - marginX, right = viewRight + marginX, top = viewTop - marginY, bottom = viewBottom + marginY;
	std::unordered_map<const Uint8*, bool> asked; // (each frame once a call: made or not)
	bool allMade = true;
	auto ask = [&](const SurfaceRaw<const Uint8> &sprite, bool inSight) {
		if (!sprite) return;
		auto known = asked.find(sprite.getBuffer());
		if (known == asked.end())
		{
			const Surface *big = upscaledRaw(sprite.getBuffer(), sprite.getPitch(), sprite.getWidth(), sprite.getHeight(), factor);
			if (big) _isoSmoothArt[sprite.getBuffer()] = big;
			known = asked.emplace(sprite.getBuffer(), big != nullptr).first;
		}
		if (inSight && !known->second) allMade = false;
	};
	for (int z = _isoLowLevel; z <= std::min(_isoViewLevel, _save->getMapSizeZ() - 1); ++z)
		for (int y = 0; y < sizeY; ++y)
			for (int x = 0; x < sizeX; ++x)
			{
				const int originX = x * 16 - y * 16 + offsetX, originY = x * 8 + y * 8 - z * 24 + offsetY;
				if (originX + 32 < left || originX > right || originY + 40 < top || originY - 40 > bottom) continue;
				Tile *tile = _save->getTile(Position(x, y, z));
				if (!tile) continue;
				const bool inSight = !(originX + 32 < viewLeft || originX > viewRight || originY + 40 < viewTop || originY - 40 > viewBottom);
				for (int part = 0; part < 4; ++part)
				{
					ask(tile->getSprite((TilePart)part), inSight);
					// (its other animation frames, ahead of them showing)
					const MapData *data = tile->getMapData((TilePart)part);
					if (!data || !data->getDataset() || !data->getDataset()->getSurfaceset()) continue;
					for (int frame = 0; frame < 8; ++frame)
					{
						const Surface *other = data->getDataset()->getSurfaceset()->getFrame(data->getSprite(frame));
						if (other) ask(SurfaceRaw<const Uint8>(other), inSight);
					}
				}
			}
	_isoSmoothFactor = factor;
	if (wanted && (allMade || _isoSmoothShare > 0.0f || now - _isoSmoothWaitSince > IsoSmoothWaitMost))
		_isoSmoothShare = std::min(1.0f, _isoSmoothShare + step);
}

bool FirstPersonView::isoSmoothHere(int x, int y) const
{
	return _isoSmoothShare >= 1.0f || (_isoSmoothShare > 0.0f && dither(x + 1, y + 2) < _isoSmoothShare);
}

/**
 * 3DIso: each tile part's art this frame for the painter (see isoArtAlong), so its rays read a table, not the tiles: the tiles
 * whose frames can reach the view (the rest none), every level. Not needed in our 3D art alone.
 */
void FirstPersonView::prepareIsoTileArt()
{
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY(), sizeZ = _save->getMapSizeZ();
	_isoArt.resize((size_t)sizeX * sizeY * sizeZ);
	const bool needed = _isoVoxelShare < 1.0f;
	// (on the card too, the same table pointing into its store of frames: see IsoCardArt)
	IsoCardArt &card = _isoCardArt;
	card.wanted = needed && Options::fpsRenderer == 1;
	if (card.wanted)
	{
		// (the smooth versions made at another factor, from another palette or "Smooth sprites" level: begun anew below)
		const Uint32 smoothKey = _colors.paletteHash * 31u + (Uint32)Options::fpsSpriteUpscale;
		if (_isoSmoothFactor && card.smoothFactor && (card.smoothFactor != _isoSmoothFactor || card.smoothKey != smoothKey)) card.slots.clear();
		if (card.slots.empty() || card.bytes.size() > IsoCardArtMostBytes)
		{
			static Uint32 generations = 0; // (every view's: the card is one for them all, and a new view's store is never an old one's)
			card.slots.clear();
			card.bytes.clear();
			card.info.clear();
			card.smoothFactor = 0;
			card.generation = ++generations;
		}
		if (_isoSmoothFactor)
		{
			card.smoothFactor = _isoSmoothFactor;
			card.smoothKey = smoothKey;
		}
		card.tiles.assign((size_t)sizeX * sizeY * sizeZ * 4, 0u);
	}
	const int offsetX = (int)_isoOffset[0], offsetY = (int)_isoOffset[1];
	const double left = isoMapX(-1), right = isoMapX(getWidth()), top = isoMapY(-1), bottom = isoMapY(getHeight());
	for (int z = 0; z < sizeZ; ++z)
		for (int y = 0; y < sizeY; ++y)
			for (int x = 0; x < sizeX; ++x)
			{
				const int index = (z * sizeY + y) * sizeX + x;
				IsoTileArt &tileArt = _isoArt[index];
				tileArt.any = false;
				const int originX = x * 16 - y * 16 + offsetX, originY = x * 8 + y * 8 - z * 24 + offsetY;
				if (!needed || originX + 32 < left || originX > right || originY + 40 + 48 < top || originY - 48 > bottom) continue;
				Tile *tile = _save->getTile(index);
				if (!tile) continue;
				const bool discovered = tile->isDiscovered(O_FLOOR);
				const int tileShade = discovered ? _map->reShade(tile) : 16;
				tileArt.westWall = (bool)tile->getSprite(O_WESTWALL);
				tileArt.top = 1 << 20;
				tileArt.bottom = -(1 << 20);
				for (int part = 0; part < 4; ++part)
				{
					IsoPartArt &art = tileArt.part[part];
					art.pixels = nullptr;
					const SurfaceRaw<const Uint8> sprite = tile->getSprite((TilePart)part);
					if (!sprite) continue;
					// (a door swinging: its new part's art waits for the swing's end, the voxel door shows meanwhile, see renderRows)
					if (!_doorSwings.empty() && (_tileFlags[index] & TileSwinging) && part != O_FLOOR && !_tileViews[index].part[part] && tile->getMapData((TilePart)part)) continue;
					art.pixels = sprite.getBuffer();
					art.width = sprite.getWidth();
					art.height = sprite.getHeight();
					art.pitch = sprite.getPitch();
					art.yOffset = tile->getYOffset((TilePart)part);
					// (the map's shade: a wall's its own, which lights a door the squad has seen beside a floor it hasn't, the rest
					// black there; OXCE's obstacles to a shot pulsing, as on the map)
					const bool wall = part == O_WESTWALL || part == O_NORTHWALL;
					art.shade = !discovered && !wall ? 16 : _map->obstacleShadeOf(tile, part, wall ? _map->wallShadeOf((TilePart)part, tile) : tileShade);
					art.smooth = nullptr;
					if (_isoSmoothFactor)
					{
						const auto smooth = _isoSmoothArt.find(art.pixels);
						if (smooth != _isoSmoothArt.end()) art.smooth = const_cast<Surface*>(smooth->second)->getSurface();
					}
					tileArt.any = true;
					tileArt.top = std::min(tileArt.top, -art.yOffset);
					tileArt.bottom = std::max(tileArt.bottom, art.height - art.yOffset);
					if (card.wanted)
					{
						const Uint32 slot = isoCardSlot(art);
						if (slot)
							card.tiles[(size_t)index * 4 + part] = slot | (Uint32)std::clamp(art.shade, 0, 31) << 16 | (Uint32)(std::clamp(art.yOffset, -128, 127) + 128) << 21
								| (part == O_NORTHWALL && tileArt.westWall ? 1u << 29 : 0u) | (part == O_OBJECT && tile->isBackTileObject(O_OBJECT) ? 1u << 30 : 0u)
								| (art.smooth ? 1u << 31 : 0u); // (its smooth frame made: the CPU's rule, see isoArtAlong)
					}
				}
			}
}

/**
 * 3DIso on the card: a part's frame's slot in the card's store (see IsoCardArt), its pixels put in the first time it's
 * asked for, and its smooth version once there is one at the store's factor. 0: no room (the frame is painted without it).
 */
Uint32 FirstPersonView::isoCardSlot(const IsoPartArt &art)
{
	IsoCardArt &card = _isoCardArt;
	if (art.width <= 0 || art.height <= 0 || art.width > 0xFFFF || art.height > 0xFFFF) return 0;
	auto found = card.slots.find(art.pixels);
	Uint32 slot;
	if (found != card.slots.end()) slot = found->second;
	else
	{
		if (card.info.size() / 4 >= 0xFFFF) return 0;
		slot = (Uint32)(card.info.size() / 4) + 1;
		card.slots.emplace(art.pixels, slot);
		card.info.push_back((Uint32)card.bytes.size());
		card.info.push_back((Uint32)art.width | (Uint32)art.height << 16);
		card.info.push_back(0u);
		card.info.push_back(0u);
		for (int row = 0; row < art.height; ++row) card.bytes.insert(card.bytes.end(), art.pixels + row * art.pitch, art.pixels + row * art.pitch + art.width);
	}
	const SDL_Surface *smooth = art.smooth;
	Uint32 *info = &card.info[(size_t)(slot - 1) * 4];
	if (smooth && !info[2] && card.smoothFactor == _isoSmoothFactor && smooth->w > 0 && smooth->h > 0 && smooth->w <= 0xFFFF && smooth->h <= 0xFFFF)
	{
		info[2] = (Uint32)card.bytes.size() + 1;
		info[3] = (Uint32)smooth->w | (Uint32)smooth->h << 16;
		const Uint8 *pixels = (const Uint8*)smooth->pixels;
		for (int row = 0; row < smooth->h; ++row) card.bytes.insert(card.bytes.end(), pixels + row * smooth->pitch, pixels + row * smooth->pitch + smooth->w);
	}
	return slot;
}

void FirstPersonView::drawIsoEffects()
{
	// In the 3D view's own order (see drawPass), over the map's overlays: what's in front of the world hides them, as the
	// map draws its own smoke and shots over its cursor and items.
	if (_isoExtras & IsoExtraSmoke) drawFire();
	if (_isoExtras & IsoExtraShots)
	{
		drawProjectilesAndExplosions();
		drawBeams();
		drawPellets();
	}
	if (_isoExtras & IsoExtraSmoke) drawSeeThroughSmoke();
	if (_isoExtras & IsoExtraEffects)
	{
		drawParticles();
		drawLightHalos();
	}
}

/**
 * The first half of a frame, the work every picture of it shares: the camera's effects (its shake, the recoil, a camera
 * feed's static spells) and axes, the flashlight's place, a chain of explosions' ripple, this frame's snapshot of the
 * tiles, the ground beyond the map (chosen once), the palette's colours for the time of day, and the particles moved
 * on (the shots followed with them). Done with the view's own camera (in VR, the head's middle).
 */
void FirstPersonView::beginFrame()
{
	// Upscaled sprites used from now on belong to this frame (see upscaled).
	_upscaleFrameStart = _upscaleClock + 1;

	// The camera for this frame: shake and recoil, then its axes; a camera's feed's static spells.
	arenaStep(); // (a developer's: see there)
	updateCameraEffects();
	if (_staticSpells) updateStaticSpell();

	updateBasis();
	arenaLog();
	// The flashlight sits on the right shoulder, a little below the eye (voxels); the dynamic
	// one just under the eye, on the weapon, so its spot falls on the crosshair.
	for (int axis = 0; axis < 3; ++axis)
	{
		_flashlightPos[axis] = _dynamicFlashlight ? (float)(_eye[axis] - _up[axis] * 1.5 + _forward[axis] * 1.0)
			: (float)(_eye[axis] + _right[axis] * 4.0 - _up[axis] * 4.0 + _forward[axis] * 2.0);
	}
	// Setting "Flashlight": Far light. Its light stands where the crosshair's line first meets the world (terrain, as the
	// view draws it), AimLightOff voxels this side of it, however far that is: found anew every frame. (And
	// AimLightLift off a floor or a ceiling the line meets at a slant: a light a voxel above the ground the line skims
	// lights next to nothing of it.)
	_aimLight = 0;
	if (_flashlight && _dynamicFlashlight && Options::fpsFlashlightReach == 1)
	{
		const float from[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
		const float direction[3] = { (float)_forwardInVoxels[0], (float)_forwardInVoxels[1], (float)_forwardInVoxels[2] };
		float surface[3];
		_aimLight = 2;
		if (visibleSurfaceAlong(from, direction, AimLightFarthest, surface))
		{
			for (int axis = 0; axis < 3; ++axis) _flashlightPos[axis] = surface[axis] - direction[axis] * AimLightOff;
			if (std::abs(direction[2]) > 0.05f) _flashlightPos[2] += direction[2] < 0.0f ? AimLightLift : -AimLightLift;
			_aimLight = 1;
		}
	}
	// A chain of explosions settled at once (setting "Instant chain explosions"): its fireballs
	// showing now, and the tiles its ripple hasn't passed over yet, as they were (see
	// FirstPersonChains.cpp).
	_chainHeld = FirstPerson::chainHeldTiles(_save);
	FirstPerson::chainFireballs(_save, _chainFireballs);
	// What the world looks like this frame: the tiles, the ground beyond the map (chosen once),
	// the palette colours for the time of day, and the particles moved on.
	snapshotTiles();
	if (!_groundChosen)
	{
		chooseGround();
	}
	updateColors();
	// VR's eyes: the weapon in hand put into the world among these tiles (see renderEyes), before the shots are
	// followed: ours leave its muzzle.
	if (_heldFrame)
	{
		placeHeldModel(*_heldFrame);
	}
	updateParticles();
}

namespace
{
/// Adds the time a pass took to its sum (OPENXCOM3D_PASS_TIME, see drawPass); nothing without sums.
struct PassTimer
{
	double *sum;
	std::chrono::steady_clock::time_point started;
	PassTimer(double *sums, int pass) : sum(sums ? sums + pass : nullptr) { if (sum) started = std::chrono::steady_clock::now(); }
	~PassTimer() { if (sum) *sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); }
};
}

/**
 * The second half of a frame: one picture of it, drawn straight into the surface's pixels (in VR an eye's, swapped in
 * for the view's own, see renderEyes). The world ray by ray, then what's in it as sprites, dots and lines, the
 * see-through smoke, the particles and the glow; the effects on the finished picture; last the marks over everything,
 * but for the units' facing rings: the caller draws those last and unlocks the surface (see draw).
 */
void FirstPersonView::drawPass()
{
	// (OPENXCOM3D_PASS_TIME: the log gets each pass's share of the main view's drawing time, averaged over 120 frames;
	// the facing rings, which the caller draws after this, are not among them.)
	static const bool passTime = getenv("OPENXCOM3D_PASS_TIME") != nullptr;
	static const char *const passNames[] = { "castAllRays", "watchUnits", "drawDecals", "drawFloorItems", "drawEdgeDots", "drawUnits", "drawFire", "drawProjectilesAndExplosions", "drawFlares", "drawBeams", "drawPellets", "drawTrajectory", "drawSeeThroughSmoke", "findUnitAtPointer", "drawParticles", "drawLightHalos", "applyScreenWash", "drawTrajectory", "applySignalGlitches", "drawBurning", "drawHurt", "drawDamageNumbers", "drawDownedIcons", "drawMarkers", "drawSprayMarks", "gpuDrawEffects" };
	static double passSums[26] = {};
	static int passFrames = 0;
	double *const passTimes = passTime && getWidth() >= 320 ? passSums : nullptr;
	// Draw straight into the surface's pixels.
	lock();
	// The world: terrain, border and sky, ray by ray (fills the depth buffer).
	{ PassTimer pass(passTimes, 0); castAllRays(); }
	// What's in the world: first the units are checked for hits and kills (for their flinches
	// and the hitmarker), then everything is drawn over the terrain as sprites, dots and lines.
	// (No unit is on the screen yet, for the see-through smoke.)
	std::fill(_unitVeil.begin(), _unitVeil.end(), (Uint8)255);
	std::fill(_smokeVeil.begin(), _smokeVeil.end(), (Uint8)0);
	std::fill(_ownColours.begin(), _ownColours.end(), (Uint8)0);
	_blendedCount = 0;
	std::fill(&_pointerUnits[0][0], &_pointerUnits[0][0] + sizeof(_pointerUnits) / sizeof(_pointerUnits[0][0]), nullptr);
	std::fill(&_pointerBodies[0][0], &_pointerBodies[0][0] + sizeof(_pointerBodies) / sizeof(_pointerBodies[0][0]), nullptr);
	std::fill(&_pointerPiles[0][0], &_pointerPiles[0][0] + sizeof(_pointerPiles) / sizeof(_pointerPiles[0][0]), nullptr);
	// (VR's second eye: the hits were watched for the first already, once a frame)
	if (!_replayPass) { PassTimer pass(passTimes, 1); watchUnits(); }
	if (_iso)
	{
		// 3DIso: the units, as they stand in the world; the rest (items, smoke, fire, shots,
		// explosions, the cursor) is the map's own sprites in its overlay (see Map::setOverlayTarget). What only
		// the 3D view has is an extra (see IsoExtra): the marks on walls and floors here, on the world, under the
		// map's overlays; its shots, smoke and effects over them (see drawIsoEffects).
		if (_isoExtras & IsoExtraEffects) drawDecals();
		if ((_isoExtras & IsoExtraLayer) || _isoMapSpritesNow) drawFloorItems(); // (extra "layer", or the art smooth: the items in 3D, not the map's)
		drawUnits();
		if (!_flares.empty()) drawFlares(); // (3DIso's tactical flares)
		return;
	}
	{ PassTimer pass(passTimes, 2); drawDecals(); }
	{ PassTimer pass(passTimes, 3); drawFloorItems(); }
	{ PassTimer pass(passTimes, 4); drawEdgeDots(); }
	{ PassTimer pass(passTimes, 5); drawUnits(); }
	{ PassTimer pass(passTimes, 6); drawFire(); }
	{ PassTimer pass(passTimes, 7); drawProjectilesAndExplosions(); }
	{ PassTimer pass(passTimes, 8); drawFlares(); }
	{ PassTimer pass(passTimes, 9); drawBeams(); }
	{ PassTimer pass(passTimes, 10); drawPellets(); }
	{ PassTimer pass(passTimes, 11); drawTrajectory(false); }
	{ PassTimer pass(passTimes, 12); drawSeeThroughSmoke(); }
	{ PassTimer pass(passTimes, 14); drawParticles(); }
	// The passes so far were recorded for the graphics card (setting "Renderer": GPU): it draws them over its rays' picture now,
	// and the bloom over that, and gives the finished one back. (Its check: the CPU's own bloom first, then the two compared.)
	if (_gpuRecording && !_gpuChecking)
	{
		// (Its own time, apart from the rays': the bins made, the primitives sent, and the wait for the card where anything is read back.)
		PassTimer pass(passTimes, 25);
		gpuDrawEffects(); // (the bloom and the screen wash with them)
	}
	else
	{
		{ PassTimer pass(passTimes, 15); drawLightHalos(); }
		// Effects on the finished picture (a camera's feed: its bad signal last).
		{ PassTimer pass(passTimes, 16); applyScreenWash(); }
		if (_gpuRecording) gpuDrawEffects();
	}
	// (VR's second eye: the unit pointed at is the first eye's, where the crosshair is)
	if (!_replayPass) { PassTimer pass(passTimes, 13); findUnitAtPointer(); }
	drawLastMarks(passTimes);
	if (passTimes && ++passFrames == 120)
	{
		std::ostringstream line;
		for (size_t pass = 0; pass < sizeof(passSums) / sizeof(passSums[0]); ++pass)
		{
			if (passSums[pass] / passFrames >= 0.005) line << " " << passNames[pass] << " " << std::lround(passSums[pass] / passFrames * 1000.0) / 1000.0;
			passSums[pass] = 0.0;
		}
		passFrames = 0;
		Log(LOG_INFO) << "OpenXcom3D: the " << getWidth() << "x" << getHeight() << " view's passes, ms:" << line.str();
	}
}

/**
 * The marks over the finished picture, drawn last in a pass (see drawPass), on the CPU whoever drew the picture: with
 * the picture kept on the graphics card they go over an empty one, which the card lays over its own (see gpuPresent,
 * and for VR's eyes presentEyesOnCard, whose check draws them a second time).
 * @param passTimes Where each one's time is summed (OPENXCOM3D_PASS_TIME), or null.
 */
void FirstPersonView::drawLastMarks(double *passTimes)
{
	// Effects on the finished picture (a camera's feed: its bad signal last). Under night vision
	// the aimed path is light in the goggles: drawn again over the green, white-green.
	if (_nightVision) { PassTimer pass(passTimes, 17); drawTrajectory(true); }
	if (_signalGlitches) { PassTimer pass(passTimes, 18); applySignalGlitches(); }
	{ PassTimer pass(passTimes, 19); drawBurning(); }
	{ PassTimer pass(passTimes, 20); drawHurt(); }
	// The icons over the downed units and the enemy markers, over everything.
	{ PassTimer pass(passTimes, 22); drawDownedIcons(); }
	{ PassTimer pass(passTimes, 23); drawMarkers(); }
	{ PassTimer pass(passTimes, 24); drawSprayMarks(); }
	// The numbers rising over the units hit (setting "Damage numbers"): over all of those too (a
	// player, GitHub issue #160: "damage numbers should be top layer, given their temporary
	// nature"; a stack of enemy markers over a unit hid its number).
	{ PassTimer pass(passTimes, 21); drawDamageNumbers(); }
}

#ifdef __ANDROID__
/**
 * The Android build's frame probe, for the tests on the headset: adds this frame's time and,
 * every ProbeSeconds, writes the stretch's frames, average and slowest frame to the game's log
 * and to the system log (tag OpenXcom3D, which `adb logcat -s OpenXcom3D` shows); the first
 * time also the processors the game may run on (Cpus_allowed_list). Each 3D view reports its own
 * (the soldier's view, and a soldier cam's or the map camera's when one is open).
 * @param drawStart The frame's start (SDL_GetPerformanceCounter).
 */
void FirstPersonView::reportFrameTime(Uint64 drawStart)
{
	const int ProbeSeconds = 5;
	const double frameMs = (SDL_GetPerformanceCounter() - drawStart) * 1000.0 / SDL_GetPerformanceFrequency();
	const Uint32 now = SDL_GetTicks();
	if (_probeFrames == 0)
	{
		_probeSince = now;
	}
	++_probeFrames;
	_probeTotalMs += frameMs;
	_probeWorstMs = std::max(_probeWorstMs, frameMs);
	if (now - _probeSince < ProbeSeconds * 1000u)
	{
		return;
	}

	// Once: the processors the game may run on (the system decides how many an app gets).
	static bool coresReported = false;
	std::ostringstream report;
	if (!coresReported)
	{
		coresReported = true;
		std::ifstream status("/proc/self/status");
		std::string line, allowed = "?";
		while (std::getline(status, line))
		{
			if (line.rfind("Cpus_allowed_list:", 0) == 0) allowed = line.substr(line.find(':') + 1);
		}
		report << "processors: " << std::thread::hardware_concurrency() << ", allowed:" << allowed << "; ";
	}

	// The stretch: frames, their average and the slowest.
	report.setf(std::ios::fixed);
	report.precision(1);
	report << "the " << getWidth() << "x" << getHeight() << " 3D view: " << _probeFrames << " frames in "
		<< (now - _probeSince) / 1000.0 << " s, average " << _probeTotalMs / _probeFrames << " ms, slowest " << _probeWorstMs << " ms";
	Log(LOG_INFO) << "OpenXcom3D: " << report.str();
	__android_log_print(ANDROID_LOG_INFO, "OpenXcom3D", "%s", report.str().c_str());
	_probeFrames = 0;
	_probeTotalMs = _probeWorstMs = 0.0;
}
#endif

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
	// of a second). None with the setting "Screen shake" off, nor in VR mode (comfort: see setComfort). The eye as
	// placed is kept: the side units are seen from doesn't shake (a player, GitHub issue #112: shooting straight down
	// at a unit, it spun round and twitched, the jitter tipping it from side to side).
	std::copy(_eye, _eye + 3, _steadyEye);
	if (_shake > 0.02f && Options::fpsScreenShake && !_comfort)
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

namespace OpenXcom
{
/**
 * 3DIso's art drawn smooth without its 3D effects (see isoMapSpritesSmooth): the map's smoke and fire as the map draws them
 * (Map::drawTerrain: the frame by the smoke's amount and the animation, in the tile's shade, fire unshaded), but each from its
 * smooth version at the view's own pixels, in the map's order (after the tile's unit, before what's in front of it), the
 * overlay's pixels laid in before it counted too.
 */
void FirstPersonView::drawIsoMapSmoke()
{
	SurfaceSet *frames = _game->getMod()->getSurfaceSet("SMOKE.PCK");
	if (!frames || _isoOverlayKeys.size() != (size_t)getWidth() * getHeight()) return;
	const int halfAnimFrame = (_map->getAnimFrame() / 2) % 4;
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	const int offsetX = (int)_isoOffset[0], offsetY = (int)_isoOffset[1];
	for (int z = _isoLowLevel; z <= std::min(_isoViewLevel, _save->getMapSizeZ() - 1); ++z)
		for (int y = 0; y < sizeY; ++y)
			for (int x = 0; x < sizeX; ++x)
			{
				const int index = (z * sizeY + y) * sizeX + x;
				Tile *tile = _save->getTile(index);
				if (!tile || !tile->getSmoke() || !tile->isDiscovered(O_FLOOR)) continue;
				int frameNumber = 0, shade = 0;
				if (!tile->getFire())
				{
					frameNumber += _save->getDepth() > 0 ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
					if (Mod::EXTENDED_SMOKE_OFFSET == 0) frameNumber += (int)std::floor(tile->getSmoke() / 6.0 - 0.1);
					else if (Mod::EXTENDED_SMOKE_OFFSET == 1) frameNumber += (int)std::floor(tile->getSmoke() / 6.0 - 0.1) * 4;
					else frameNumber += (tile->getSmoke() - 1) / 5 * 4;
					shade = _map->reShade(tile);
				}
				const int phase = halfAnimFrame + tile->getAnimationOffset();
				frameNumber += phase > 3 ? phase - 4 : phase;
				const Surface *frame = frames->getFrame(frameNumber);
				if (!frame) continue;
				drawIsoMapSprite(frame, x * 16 - y * 16 + offsetX, x * 8 + y * 8 - z * 24 + offsetY, (Uint32)index * 16 + 11, shade, true, true);
			}
}

void FirstPersonView::drawIsoMapSprite(const Surface *frame, int originX, int originY, Uint32 key, int shade, bool mapShaded, bool overlayCounts)
{
	if (!frame) return;
	const bool overlay = overlayCounts && _isoOverlayKeys.size() == (size_t)getWidth() * getHeight();
	const SurfaceRaw<const Uint8> plain(frame);
	const int factor = _isoSmoothFactor;
	const Surface *smooth = factor > 0 && _isoSmoothShare > 0.0f ? upscaledRaw(plain.getBuffer(), plain.getPitch(), plain.getWidth(), plain.getHeight(), factor) : nullptr;
	const SDL_Surface *big = smooth ? const_cast<Surface*>(smooth)->getSurface() : nullptr;
	const int left = std::max(0, (int)std::floor(isoViewX(originX))), right = std::min(getWidth(), (int)std::ceil(isoViewX(originX + plain.getWidth())));
	const int top = std::max(0, (int)std::floor(isoViewY(originY))), bottom = std::min(getHeight(), (int)std::ceil(isoViewY(originY + plain.getHeight())));
	for (int viewY = top; viewY < bottom; ++viewY)
	{
		const double spriteY = isoMapY(viewY) - originY;
		if (spriteY < 0.0 || spriteY >= plain.getHeight()) continue;
		Uint8 *row = getRaw(0, viewY);
		for (int viewX = left; viewX < right; ++viewX)
		{
			const double spriteX = isoMapX(viewX) - originX;
			if (spriteX < 0.0 || spriteX >= plain.getWidth()) continue;
			const size_t at = (size_t)viewY * getWidth() + viewX;
			if (key <= _isoKeys[at] || (overlay && key <= _isoOverlayKeys[at])) continue; // (the map draws something there after it)
			Uint8 color;
			if (big && isoSmoothHere(viewX, viewY))
			{
				color = ((const Uint8*)big->pixels)[std::min(big->h - 1, (int)(spriteY * factor)) * big->pitch + std::min(big->w - 1, (int)(spriteX * factor))];
			}
			else color = plain.getBuffer()[(int)spriteY * plain.getPitch() + (int)spriteX];
			if (color) row[viewX] = mapShaded ? isoShaded(color, shade) : color;
		}
	}
}

/**
 * 3DIso's own cursor (extra "layer", the map's left out of its overlay): as the map draws it (Map::drawTerrain), on each of its
 * tiles (a big unit's cursor covers two each way): on the view level its back (behind the tile's unit) and its front (in front),
 * a red box, over a unit seen yellow and red by turns; aiming, red crosshairs, over a unit seen yellow ones turning; throwing,
 * psi and the waypoints their own mark over the box; on the levels under it a blue box. (The accuracy's number is the map's,
 * in its overlay.) Where the picture is the original art at the map's slant: the map's own sprites (CURSOR.PCK), at the
 * view's pixels, exact. In our 3D art, turned or tilted: the original built in 3D, a box in those sprites' colours by the
 * same turns and their crosshairs and marks standing up in the world (the owner: "that cursor is for classic iso art").
 */
void FirstPersonView::drawIsoCursor()
{
	const CursorType cursor = _map->getCursorType();
	BattlescapeState *state = _save->getBattleState();
	SurfaceSet *frames = _game->getMod()->getSurfaceSet("CURSOR.PCK");
	if (cursor == CT_NONE || (state && state->getMouseOverIcons()) || !frames) return;
	Position selected;
	_map->getSelectorPosition(&selected);
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY(), cursorSize = std::max(1, _map->getCursorSize());
	const int level = _map->getCamera()->getViewLevel(); // (the map's view level: with all levels shown, not the top)
	const int animFrame = _map->getAnimFrame(), halfAnimFrame = (animFrame / 2) % 4, halfAnimFrameRest = animFrame % 2;
	// (the cursor built in 3D blinks slower, the owner: "blinks too fast": a big box swapping yellow and red ten times a second
	// reads as a flicker. Its steps at the original's slowest, the marks' 0.4 s: the box's colours and the crosshairs'
	// frames. The original's own sprites keep the map's pace.)
	const int slowStep = animFrame / 4, slowCrosshair = slowStep % 4, slowBoxRest = slowStep % 2;
	// (a frame's own colour: the one most of its pixels have, for the box turned)
	auto colourOf = [](const Surface *frame) {
		static std::unordered_map<const Surface*, Uint8> known;
		auto found = known.find(frame);
		if (found != known.end()) return found->second;
		int counts[256] = {};
		const SurfaceRaw<const Uint8> raw(frame);
		for (int y = 0; y < raw.getHeight(); ++y)
			for (int x = 0; x < raw.getWidth(); ++x) ++counts[raw.getBuffer()[y * raw.getPitch() + x]];
		int best = 1;
		for (int index = 1; index < 256; ++index) if (counts[index] > counts[best]) best = index;
		return known[frame] = (Uint8)best;
	};
	const bool artFloors = _isoVoxelShare < 0.5f;
	// (the original's own sprites where the picture is the original art at the map's slant: exact; our 3D art, turned or
	// tilted: built in 3D, see below; the owner's word)
	const bool spritesExact = _iso && _isoVoxelShare < 0.5f;
	for (int dy = 0; dy < cursorSize; ++dy)
		for (int dx = 0; dx < cursorSize; ++dx)
		{
			const int x = selected.x + dx, y = selected.y + dy;
			if (x < 0 || y < 0 || x >= sizeX || y >= sizeY) continue;
			for (int z = _isoLowLevel; z <= std::min(level, _save->getMapSizeZ() - 1); ++z)
			{
				Tile *tile = _save->getTile(Position(x, y, z));
				if (!tile) continue;
				const BattleUnit *unit = tile->getUnit();
				const bool unitSeen = z == level && unit && (unit->getVisible() || _save->getDebugMode());
				int back, front;
				if (z < level) { back = 2; front = 5; } // (blue box)
				else if (cursor != CT_AIM) { back = unitSeen ? halfAnimFrameRest : 0; front = unitSeen ? 3 + halfAnimFrameRest : 3; }
				else back = front = unitSeen ? 7 + halfAnimFrame : 6;
				const int index = (z * sizeY + y) * sizeX + x;
				static const int marks[6] = { 0, 0, 0, 11, 13, 15 };
				const int mark = z == level && cursor > CT_AIM && cursor < 6 ? marks[cursor] + (animFrame / 4) % 2 : -1;
				if (spritesExact)
				{
					const int originX = x * 16 - y * 16 + (int)_isoOffset[0], originY = x * 8 + y * 8 - z * 24 + (int)_isoOffset[1];
					drawIsoMapSprite(frames->getFrame(back), originX, originY, (Uint32)index * 16 + 2, 0, false, false);
					drawIsoMapSprite(frames->getFrame(front), originX, originY, (Uint32)index * 16 + 14, 0, false, false);
					// (throwing, psi and the blaster's waypoints: their mark over it, turning slowly)
					if (mark >= 0) drawIsoMapSprite(frames->getFrame(mark), originX, originY, (Uint32)index * 16 + 14, 0, false, false);
					continue;
				}
				// The original's cursor built in 3D: its box round the tile (aiming, only its floor), the lines as thick as the
				// sprite's, in its colour shading down from the top as the sprite's edges do (the unit's yellow and red by turns);
				// the crosshairs and the marks the original's own frames, standing up in the world from the tile's middle.
				const Surface *colourFrame = frames->getFrame(z < level ? 2 : cursor == CT_AIM ? (unitSeen ? 7 + slowCrosshair : 6) : unitSeen ? slowBoxRest : 0);
				if (!colourFrame) continue;
				const Uint8 colour = colourOf(colourFrame), group = colour & 0xF0, shade = colour & 15;
				auto shaded = [&](int step) { return (Uint8)(group | std::clamp((int)shade + step, 0, 15)); };
				const float floor = z * 24.0f + (artFloors ? 0.0f : (float)GroundTop) - tile->getTerrainLevel() + 0.5f;
				// (a voxel inside the tile each side, as the sprite's box is)
				const float x0 = x * 16.0f + 1.0f, y0 = y * 16.0f + 1.0f, z0 = floor - 0.5f, x1 = x0 + 14.0f, y1 = y0 + 14.0f, z1 = z * 24.0f + 24.0f;
				const float corners[8][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 }, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
				const int thickness = std::max(1, (int)std::lround(_isoZoomTotal));
				if (z < level)
				{
					// (a level under the cursor's: as the sprite, its floor and its uprights dotted)
					auto dotted = [&](const float *from, const float *to) {
						const float length = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
						const int dots = std::max(1, (int)std::lround(length / 3.0f));
						for (int dot = 0; dot < dots; ++dot)
						{
							const float a = dot / (float)dots, b = (dot + 0.3f) / dots;
							const float p[3] = { from[0] + (to[0] - from[0]) * a, from[1] + (to[1] - from[1]) * a, from[2] + (to[2] - from[2]) * a };
							const float q[3] = { from[0] + (to[0] - from[0]) * b, from[1] + (to[1] - from[1]) * b, from[2] + (to[2] - from[2]) * b };
							drawLine3D(p, q, colour, thickness, 1.0f, 0.0f, 0, 1 << 30, false, true);
						}
					};
					for (int edge = 0; edge < 4; ++edge) dotted(corners[edge], corners[(edge + 1) % 4]);
					for (int edge = 0; edge < 4; ++edge) dotted(corners[4 + edge], corners[edge]);
					continue;
				}
				auto line = [&](const float *from, const float *to, Uint8 lineColour) {
					// (the part behind what stands there faint, the rest solid)
					drawLine3D(from, to, lineColour, thickness, 0.35f, 0.0f, 0, 1 << 30, false, true);
					drawLine3D(from, to, lineColour, thickness, 1.0f, 0.0f, 0, 1 << 30, false, false);
				};
				for (int edge = 0; edge < 4; ++edge) line(corners[edge], corners[(edge + 1) % 4], shaded(1)); // (the floor)
				for (int edge = 0; edge < 4; ++edge) line(corners[4 + edge], corners[4 + (edge + 1) % 4], shaded(-1)); // (the top)
				for (int edge = 0; edge < 4; ++edge)
				{
					// (the uprights, darker down: in four steps)
					for (int step = 0; step < 4; ++step)
					{
						const float from[3] = { corners[4 + edge][0], corners[4 + edge][1], z1 + (z0 - z1) * step / 4.0f };
						const float to[3] = { corners[4 + edge][0], corners[4 + edge][1], z1 + (z0 - z1) * (step + 1) / 4.0f };
						line(from, to, shaded(step - 1));
					}
				}
				if (cursor == CT_AIM)
				{
					// Aiming: a target inside the box (the owner: "draw a target inside from bolder lines that adapts more
					// beautifully to 3D"; the original's crosshairs as a picture standing up looked strange turned): the
					// original's crosshairs' shape, a ring and four spokes pointing in, thick outside and thinner in, in bolder
					// lines than the box's, in the box's middle, always facing the camera however it is turned and tilted (in
					// the plane of its right and up), over everything as the original's are. On a unit seen, yellow, the spokes
					// going in and out by the cursor's steps (as the original's frames do); else red and still.
					const float centre[3] = { x0 + 7.0f, y0 + 7.0f, (z0 + z1) * 0.5f };
					float across[3], upward[3]; // (the camera's right and up as world steps: z is ZScale times taller in its frame)
					for (int axis = 0; axis < 3; ++axis)
					{
						across[axis] = (float)(_right[axis] / (axis == 2 ? ZScale : 1.0));
						upward[axis] = (float)(_up[axis] / (axis == 2 ? ZScale : 1.0));
					}
					auto at = [&](float rightward, float up, float *point) {
						for (int axis = 0; axis < 3; ++axis) point[axis] = centre[axis] + across[axis] * rightward + upward[axis] * up;
					};
					const int bold = thickness + 1;
					const Uint8 ringColour = shaded(-1), spokeColour = shaded(-2);
					constexpr int RingSegments = 40;
					constexpr float RingRadius = 6.0f, SpokeOut = 8.0f;
					static const float spokeIn[4] = { 1.5f, 2.6f, 3.7f, 2.6f };
					const float inner = unitSeen ? spokeIn[slowCrosshair] : 2.6f;
					for (int segment = 0; segment < RingSegments; ++segment)
					{
						const float a = segment * 2.0f * (float)Pi / RingSegments, b = (segment + 1) * 2.0f * (float)Pi / RingSegments;
						float from[3], to[3];
						at(RingRadius * std::cos(a), RingRadius * std::sin(a), from);
						at(RingRadius * std::cos(b), RingRadius * std::sin(b), to);
						drawLine3D(from, to, ringColour, bold, 1.0f, 0.0f, 0, 1 << 30, false, true);
					}
					for (int spoke = 0; spoke < 4; ++spoke)
					{
						const float alongX = spoke == 0 ? 1.0f : spoke == 2 ? -1.0f : 0.0f, alongY = spoke == 1 ? 1.0f : spoke == 3 ? -1.0f : 0.0f;
						const float halfway = (SpokeOut + inner) * 0.5f;
						float out[3], mid[3], in[3];
						at(alongX * SpokeOut, alongY * SpokeOut, out);
						at(alongX * halfway, alongY * halfway, mid);
						at(alongX * inner, alongY * inner, in);
						drawLine3D(out, mid, spokeColour, bold + 1, 1.0f, 0.0f, 0, 1 << 30, false, true);
						drawLine3D(mid, in, spokeColour, bold, 1.0f, 0.0f, 0, 1 << 30, false, true);
					}
				}
				const double middle[3] = { x0 + 8.0, y0 + 8.0, floor };
				if (mark >= 0) drawIsoCursorFrame(frames->getFrame(mark), middle);
			}
		}
}

/**
 * 3DIso's cursor built in 3D (see drawIsoCursor): one of the original cursor's frames standing up in the world, its floor's
 * middle (the frame's pixel 16, 32) on a tile's floor middle, at the picture's scale (smooth when big), over everything.
 */
void FirstPersonView::drawIsoCursorFrame(const Surface *frame, const double floorMiddle[3])
{
	double screenX, screenY, depth;
	if (!frame || !projectToScreen(floorMiddle, screenX, screenY, depth)) return;
	const double scale = std::max(1.0, _isoZoomTotal);
	const SurfaceRaw<const Uint8> plain(frame);
	const int factor = scale >= UpscaleFrom && Options::fpsSpriteUpscale > 0 ? spriteUpscaleFactor() : 0;
	const Surface *smooth = factor ? upscaledRaw(plain.getBuffer(), plain.getPitch(), plain.getWidth(), plain.getHeight(), factor) : nullptr;
	const SDL_Surface *big = smooth ? const_cast<Surface*>(smooth)->getSurface() : nullptr;
	const double left = screenX - 16.0 * scale, top = screenY - 32.0 * scale;
	const int x0 = std::max(0, (int)std::floor(left)), x1 = std::min(getWidth(), (int)std::ceil(left + plain.getWidth() * scale));
	const int y0 = std::max(0, (int)std::floor(top)), y1 = std::min(getHeight(), (int)std::ceil(top + plain.getHeight() * scale));
	for (int viewY = y0; viewY < y1; ++viewY)
	{
		const double spriteY = (viewY + 0.5 - top) / scale;
		if (spriteY < 0.0 || spriteY >= plain.getHeight()) continue;
		Uint8 *row = getRaw(0, viewY);
		for (int viewX = x0; viewX < x1; ++viewX)
		{
			const double spriteX = (viewX + 0.5 - left) / scale;
			if (spriteX < 0.0 || spriteX >= plain.getWidth()) continue;
			const Uint8 color = big ? ((const Uint8*)big->pixels)[std::min(big->h - 1, (int)(spriteY * factor)) * big->pitch + std::min(big->w - 1, (int)(spriteX * factor))]
				: plain.getBuffer()[(int)spriteY * plain.getPitch() + (int)spriteX];
			if (color) row[viewX] = color;
		}
	}
}

/**
 * 3DIso: when the map draws a tile part, in its order (see Map::overlayKeys): the tile's index times 16 plus its place in
 * the tile (floor 0, the cursor's back 2, west wall 4, north wall 6, an object behind the units 8, items 9, the unit 10, smoke
 * and fire 11, an object in front 12, the cursor's front 14).
 */
Uint32 FirstPersonView::isoKeyOf(int tileIndex, int part) const
{
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	const Tile *tile = _save->getTile(Position(tileIndex % sizeX, (tileIndex / sizeX) % sizeY, tileIndex / (sizeX * sizeY)));
	const int slot = part == O_FLOOR ? 0 : part == O_WESTWALL ? 4 : part == O_NORTHWALL ? 6 : (tile && tile->isBackTileObject(O_OBJECT)) ? 8 : 12;
	return (Uint32)tileIndex * 16 + slot;
}
}
