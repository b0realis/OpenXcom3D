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
 * FirstPersonView.h: the 3D view, the software ray caster that draws the battlescape in first
 * person (its code is in the FirstPersonView*.cpp files, one per topic).
 */
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../Battlescape/Position.h"
#include "../Engine/InteractiveSurface.h"
#include "FirstPersonParticles.h"
#include "FirstPersonVoxelModel.h"

namespace OpenXcom
{

class Game;
class Map;
class SavedBattleGame;
class MapData;
class BattleUnit;
class Tile;
class Explosion;
class Projectile;
class RuleItem;
class BattleItem;

/**
 * First-person view of the battlescape.
 * Software ray caster over the LOFT voxel model (the same 16x16x24 voxels per tile
 * that line of fire uses), textured by projecting each tile part's isometric sprite
 * onto its voxels, lit from the game's own per-tile light, under a day/night sky,
 * with the map's ground running on to the horizon past a taped-off map border.
 * Draws straight into an 8-bit surface, so palette shading works as usual.
 *
 * The class is big, so its code is spread over one file per topic, and this declaration
 * follows the same order:
 *
 *   FirstPersonView.cpp               creating it, the camera, and drawing a frame (draw)
 *   FirstPersonViewTerrain.cpp        the voxel models of the map, doors, the per-frame tile snapshot
 *   FirstPersonViewRayCasting.cpp     colours, light and shadow, the sky, and the threaded ray caster
 *                                     (the ray walk itself is in FirstPersonViewRayWalk.h)
 *   FirstPersonViewSprites.cpp        units, items and markers drawn as sprites in the 3D world
 *   FirstPersonViewUpscaling.cpp      smoother sprites up close, made on a worker thread
 *   FirstPersonViewEffects.cpp        particles: explosions, fire, smoke, weather, UFO devices, glows
 *   FirstPersonViewShots.cpp          shots in flight, beams, impacts, bullet holes, explosions' sprites
 *   FirstPersonViewFlares.cpp         tactical flares and their light
 *   FirstPersonViewUfo.cpp            UFOs (an option): steady hulls with glowing lights, coloured light, an aura
 *   FirstPersonViewScreenEffects.cpp  effects on the screen itself: flames, blood, the hurt arc
 *
 * Constants and helpers the files share are in FirstPersonViewShared.h.
 */
class FirstPersonView : public InteractiveSurface
{
public:
	// =====================================================================================
	// Data the ray caster works on (public: the ray walk in FirstPersonViewRayWalk.h uses it)
	// =====================================================================================

	/// Colour model of one MapData record: palette index per voxel, 0 = empty.
	struct VoxelModel
	{
		Uint8 color[24][16][16]; // [z][y][x]
		Uint16 glass[24][16];    // [z][y], bit x: window glass, seen through
		Uint8 min[3], max[3];    // inclusive bounds of solid voxels (x, y, z)
		bool empty;
		float glow[3]; // centre of the brightest voxels in the top third (a lamp's bulb), in the tile
		int glowCount;
		Uint8 glowColor;     // one of those bright colours
		Uint8 glowMinZ;      // only voxels from this height up shine
		Uint8 plan[16][16];  // [y][x] the colour seen from above: an underside shows this (the iso art never shows undersides)
		Uint8 ceiling;       // a thin plate's (floor, roof) underside: its most common top colour, 0 if not a plate
		Uint8 east[24][16];  // [z][y] the colour seen from the east: a west-facing side shows this (the iso art shows only east and south sides)
		Uint8 south[24][16]; // [z][x] the colour seen from the south: a north-facing side shows this
		// Only on a UFO part's steady model (the UFO lights option, see FirstPersonViewUfo.cpp):
		const Uint16 (*lit)[16];        // [z][y], bit x: the voxel is one of the part's lights; null on other models
		const Uint8 (*exposed)[16][16]; // [z][y][x]: the voxel's open faces (bit per face: +x -x +y -y +z -z), for smooth hull shading
		Uint8 pulseKind;                // how its lights pulse: 1 small lights, 2 a big lit panel (a lift pad), 0 none
	};
	/// Per-frame snapshot of what a ray needs to know about a tile.
	struct TileView
	{
		const VoxelModel *part[4];
		const VoxelModel *next[4]; // an animated part's next frame, which it dissolves into
		Uint8 min[3], max[3];
		Uint8 light;     // the game's shade for the tile: 0 brightest .. 15 darkest
		bool discovered; // the floor (and the object on it) has been seen
		Uint8 seen;      // bit per part seen: walls are seen from either side, like on the map
		bool empty;
		Sint16 swing[2]; // door swings reaching into this tile this frame, -1 none
		Uint8 emissive;  // bit per part: a light source, whose bright voxels shine
		Uint8 ufoGlow;   // a UFO's light here (the UFO lights option, once the UFO has been seen): 0 none .. 255 full
	};
	/// A door swinging open, as a thin slab turning around its hinge.
	struct DoorSwing
	{
		float hinge[2];          // world x, y
		float dir[2];            // unit vector along the door
		float z0, z1;            // world z range
		const VoxelModel *model; // the closed door, for its colours
		bool westWall;           // closed door was a west wall (runs along y) rather than a north wall
	};
	/// What a ray hit inside the map.
	struct Hit
	{
		Uint8 color;
		float t;
		int axis;       // axis of the face it entered through, -1 if unknown
		int tile;       // tile index
		int part;       // tile part hit, -1 for a swinging door
		Uint8 voxel[3]; // which voxel of the part (x, y, z in the tile)
		bool glass;     // the ray went through window glass on the way
		bool emissive;  // hit the bright part of a light source
	};
	/// Palette colours picked for the current palette and time of day.
	struct SceneColors
	{
		Uint8 sky[32];  // horizon .. zenith
		Uint8 cloud[8]; // lit .. shadowed
		Uint8 haze, star, moon, sun, tapeYellow, tapeBlack, post;
		float night; // 0 day .. 1 night
		bool underwater;
		int ambient;        // shade of open ground under the sky
		float faceShade[6]; // extra shade per face normal: +x -x +y -y +z -z
		float sunDir[3], moonDir[3];
		float cloudTime;
		Uint8 glassTint[256]; // each colour as seen through glass
		Uint32 paletteHash;   // palette the tint table was built for
		Uint8 fire[4];        // white-hot .. dark red
		Uint8 blood[2];       // splatter on the screen: dark, bright
		Uint8 smoke, spark, dust, rain, snow, sand, firefly, bubble;
		Uint8 marker, marine;
		Uint8 redTint[256];    // each colour washed with red (hurt, dying)
		Uint8 blueTint[256];   // ... with dark blue (passing out)
		Uint8 orangeTint[256]; // ... with orange (falling under mind control)
		Uint8 whiteTint[256];  // ... with white (a breakdown)
		Uint8 greenTint[256];  // ... with a sickly green (turning into a zombie)
		Uint8 grey[4];         // TV static
		Uint8 flareTint[256];  // each colour in a flare's red light
		Uint8 flareFire[3];    // flare flame: red, orange, yellow
	};

	// =====================================================================================
	// Setting the view up
	// =====================================================================================

	/// Creates the first-person view.
	FirstPersonView(Game *game, int width, int height, int x, int y);
	/// Cleans up the first-person view.
	~FirstPersonView();
	/// Sets the battle to render.
	void setSavedBattleGame(SavedBattleGame *save);
	/// Sets the isometric map, for projectiles and explosions.
	void setMap(Map *map) { _map = map; }
	/// Uses another view's voxel models instead of building its own (a small extra view).
	void shareCache(FirstPersonView *owner) { _sharedModelsOwner = owner; }
	/// Turns particles (sparks, smoke, weather...) on or off for this view.
	void setParticles(bool on) { _particlesOn = on; }
	/// Blends the game's per-tile light smoothly instead of tile by tile.
	void setSmoothLighting(bool smooth) { _smoothLighting = smooth; }
	/// Sets how far the view reaches, in tiles.
	void setMaxDistance(int tiles);
	/// Sets the horizontal field of view.
	void setFieldOfView(double degrees);
	/// Draws the enemy markers this many times bigger (big screens).
	void setUiScale(int scale) { _uiScale = scale; }

	// =====================================================================================
	// The camera
	// =====================================================================================

	/// Sets the unit being looked through (not drawn).
	void setCameraUnit(const BattleUnit *unit) { _cameraUnit = unit; }
	/// Places the eye, in voxels.
	void setEye(double x, double y, double z);
	/// Gets the eye, in voxels.
	void getEye(double out[3]) const;
	/// Looks along a unit direction (0 = north, clockwise), level.
	void lookAlong(int unitDirection);
	/// Turns the camera.
	void rotate(double yawDegrees, double pitchDegrees);
	/// Points the camera at a point in voxels.
	void lookAt(const double target[3]);
	/// Gets the look direction around the vertical axis, in degrees (0 = east, 90 = south).
	double getYawDegrees() const;
	/// Gets how far the view looks up (negative: down), in degrees.
	double getPitchDegrees() const;
	/// Gets the (normalized) direction of the ray through a pixel of the view.
	void getRayDirection(int pixelX, int pixelY, double out[3]);

	// =====================================================================================
	// What to show
	// =====================================================================================

	/// Marks units (the visible enemies) with their numbers, over everything.
	void setMarkers(const std::vector<std::pair<const BattleUnit*, int>> &markers) { _markers = markers; }
	/// Leaves out one tile's floor (the lift platform being ridden through), or none (index -1).
	void setHiddenFloor(int tileIndex) { _hiddenFloor = tileIndex; }
	/// Turns the shoulder flashlight on or off.
	void setFlashlight(bool on) { _flashlight = on; }
	/// Night vision like the isometric map's: brighter, in one colour (group + 1, 0 off).
	void setNightVision(int color) { _nightVision = color; }
	/// Where the weapon in hand's muzzle is on the view (pixels): our own shots flash and start their trails there.
	void setMuzzlePoint(double x, double y, bool valid) { _muzzleScreen[0] = x; _muzzleScreen[1] = y; _muzzleValid = valid; }
	/// Dual wield: where the other gun's muzzle is on the view.
	void setOffMuzzlePoint(double x, double y, bool valid) { _offHandMuzzleScreen[0] = x; _offHandMuzzleScreen[1] = y; _offHandMuzzleValid = valid; }
	/// Throws a tactical flare from the soldier's shoulder towards a point (a short toss under the sea).
	void throwFlare(const float target[3]);

	// =====================================================================================
	// Effects on the screen
	// =====================================================================================

	/// The soldier was hurt: an arc on the screen's edge towards where it came from (a world
	/// point, or none), and with a heavy or close hit blood splattered over the view.
	void showHurt(const double *from, float amount, bool splatter);
	/// Reddens the screen edges (0 none .. 1 strong), for a soldier badly hurt.
	void setVignette(float amount) { _vignette = amount; }
	/// Shakes the camera (explosions, hits); stronger shakes win.
	void addShake(float amount) { _shake = std::max(_shake, amount); }
	/// Washes the view with a colour (0 none .. 1 full): 0 red (hurt, killed), 1 dark blue (unconscious),
	/// 2 orange (mind control), 3 white (a breakdown), 4 sickly green (turning into a zombie); `red`
	/// is the share of the washed pixels washed red instead.
	void setTint(float amount, int color = 0, float red = 0.0f) { _tintAmount = amount; _tintKind = color; _tintRedShare = red; }
	/// The soldier is on fire: flames lick up from the bottom of the view (0 none .. 1 fierce).
	void setBurning(float amount) { _burningAmount = amount; }
	/// A burst of flames (catching fire), 0..1.
	void flareUp(float amount) { _flameBurst = std::max(_flameBurst, amount); }
	/// TV static over the view (a helmet camera losing its signal), 0 none .. 1 all.
	void setStatic(float amount) { _staticAmount = amount; }

	// =====================================================================================
	// What the first-person mode asks the view
	// =====================================================================================

	/// Is the door in this wall still moving: a door swinging open, or a UFO door sliding?
	bool isDoorMoving(const Position &pos, int part);
	/// The stereo angle of a sound from a tile, for this camera as the listener (0 ahead, 90 right).
	int soundAngle(const Position &tile) const;
	/// The game's shade (0 bright .. 15 dark) where the camera is.
	int getEyeShade() const;
	/// How far the view is kicked up by recoil right now, in degrees.
	float getRecoilKick() const { return _recoilKick; }
	/// When an enemy in sight was last hit, and whether that hit killed it (for the hitmarker).
	Uint32 getHitMarkTick() const { return _hitMarkTick; }
	bool getHitMarkKill() const { return _hitMarkKill; }
	/// When an enemy shot last whizzed past close by (0 never), and from which side.
	Uint32 getNearMissTick() const { return _nearMissTick; }
	void getNearMissDirection(double out[2]) const { out[0] = _nearMissDirection[0]; out[1] = _nearMissDirection[1]; }
	/// Where the last enemy shot came from and when it ended (0 none yet).
	Uint32 getEnemyShotEnd(double from[3]) const { std::copy(_enemyShotFrom, _enemyShotFrom + 3, from); return _enemyShotEnd; }
	/// When our own last shot left the muzzle (for the weapon in hand's flash).
	Uint32 getOwnShotTick() const { return _ownShotTick; }
	/// The ammo our own last shot fired (null if unknown).
	const RuleItem *getOwnShotAmmo() const { return _ownShotAmmo; }
	/// Dual wield: when the other gun's last round left its muzzle.
	Uint32 getPartnerShotTick() const { return _partnerShotTick; }
	/// Dual wield: the ammo the other gun's last round fired (null if unknown).
	const RuleItem *getPartnerShotAmmo() const { return _partnerShotAmmo; }

	/// Renders the view.
	void draw() override;

private:
	// =====================================================================================
	// The battle, the camera and the frame (FirstPersonView.cpp)
	// =====================================================================================

	Game *_game;                                        // the game: mod rules, options, palette
	SavedBattleGame *_save = nullptr;                   // the battle drawn
	Map *_map = nullptr;                                // the isometric map: projectiles and explosions come from it
	const BattleUnit *_cameraUnit = nullptr;            // the unit looked through (not drawn)
	double _eye[3] = {};                                // the eye, in voxels
	double _yaw = 0.0, _pitch = 0.0;                    // radians; yaw 0 = +x (east), pi/2 = +y (south)
	double _forward[3] = {}, _right[3] = {}, _up[3] = {}; // camera axes, in true proportions (see ZScale)
	double _forwardInVoxels[3] = { 1.0, 0.0, 0.0 };     // the view direction in the voxel grid
	double _halfFovTangent;                             // tan(horizontal fov / 2)
	float _maxDistance = 60.0f * Position::TileXY;      // in voxels
	float _shake = 0.0f;                                // camera shake, voxels
	Uint32 _shakeLastTick = 0;                          // when the shake was last updated
	float _recoilKick = 0.0f;                           // recoil: the view kicked up after your own shot, degrees
	int _nightVision = 0;                               // night vision colour group + 1, 0 off
	std::vector<float> _depthBuffer;                    // per pixel, how far away what's drawn there is (sprites and effects test against it)
	std::vector<Uint8> _glowMask;                       // pixels showing something that shines (lit voxels, sparks)

	/// Works out the camera's axes from its yaw and pitch.
	void updateBasis();
	/// This frame's camera shake, recoil kick and muzzle flash light.
	void updateCameraEffects();
	/// Builds every voxel model the battle can show, before it's needed.
	void preloadModels();

	// =====================================================================================
	// The map as voxels: models, doors, the tile snapshot (FirstPersonViewTerrain.cpp)
	// =====================================================================================

	/// Sprite frames of a MapData record, to tell which animation frame a tile shows.
	struct Frames
	{
		int sprite[8];
		const Uint8 *buffer[8];
		bool animated;
		const VoxelModel *steady = nullptr; // a UFO part's steady 3D-only model (the UFO lights option), null otherwise
	};
	/// A door that opened recently and is still swinging.
	struct DoorAnimation
	{
		int tile, part, hiddenPart;
		const VoxelModel *model;
		Uint32 start;
		bool away; // swings into the neighbour tile, away from a camera on the door's own side
	};
	std::unordered_map<Uint64, VoxelModel> _models;     // by MapData, sprite and cut-out
	std::unordered_map<const MapData*, Frames> _frames; // by MapData: its animation frames
	FirstPersonView *_sharedModelsOwner = nullptr;      // another view whose voxel models this one uses, or null
	const VoxelModel *_groundModel = nullptr;           // floor repeated outside the map
	bool _groundChosen = false;                         // the outside ground has been picked
	std::vector<TileView> _tileViews;                   // this frame's snapshot of every tile: what the rays read
	int _hiddenFloor = -1;                              // a floor left out: the lift platform the camera rides through
	std::vector<const MapData*> _wallsLastFrame[2];     // west/north wall of each tile last frame
	std::vector<DoorAnimation> _doorAnimations;         // doors swinging open
	std::unordered_map<int, Uint32> _ufoDoorOpening;    // UFO door part (tile * 4 + part) -> when it started sliding
	std::vector<DoorSwing> _doorSwings;                 // this frame
	std::vector<const MapData*> _partsLastFrame[4];     // every tile part last frame, to notice destruction
	std::vector<Uint8> _burningLastFrame;               // tiles on fire last frame: burnt out, they leave a scorch
	// animated tiles step a frame every CalmFrameTime ms on the view's own calm clock
	int _calmFrame = 0;                                 // the animation frame (0..7) animated tiles show now
	float _animationBlend = 0.0f;                       // how far they have dissolved into the next one (0..1, eased)
	// light sources and UFO devices near the camera, found while taking the snapshot
	struct LightSpot { float pos[3]; float strength; Uint8 color; };
	std::vector<LightSpot> _nearbyLights; // light sources near the camera this frame
	struct DeviceSpot { float pos[3]; float half[3]; bool power; };
	std::vector<DeviceSpot> _nearbyDevices; // UFO power sources and navigation consoles nearby
	// crafts and UFOs are symmetric: a side the iso art never shows (facing west or north) takes
	// its colours from the mirrored side it does show
	struct Mirror
	{
		int lo[3], size[3];           // the craft's voxels: first world voxel and extent per axis
		int sum[2];                   // mirror line across x and across y (twice its world coordinate)
		bool symmetric[2];            // the craft's shape mirrors across x / across y
		std::vector<int> start[2];    // per voxel row along x / y: where its surfaces begin in pos
		std::vector<Sint16> pos[2];   // voxels on those rows whose east / south face is open, in order
		std::vector<Sint16> outer[2]; // per row: the craft's first voxel from the west / north, -1 none
	};
	std::vector<Mirror> _mirrors;      // the crafts and UFOs on the map
	std::vector<Sint16> _mirrorOfTile; // per tile: its craft in _mirrors, -1 none

	/// Gets (building on first use) the voxel model of a MapData record shown with a sprite.
	const VoxelModel *getModel(const MapData *mapData, int spriteIndex = -1, bool cutout = false);
	/// Builds a voxel model: shapes from the LOFT templates, colours from the isometric sprite.
	void buildModel(const MapData *mapData, int spriteIndex, bool cutout, VoxelModel &model) const;
	/// Gets the sprite frames of a MapData record, and whether it animates.
	const Frames &getFrames(const MapData *mapData);
	/// Starts the swing of doors that just opened on a tile.
	void trackDoors(int index, Tile *tile);
	/// Picks the ground that continues past the map border.
	void chooseGround();
	/// Takes this frame's snapshot of every tile, for the rays.
	void snapshotTiles();
	/// What changed on a tile since the last frame: doors, burnt-out fires, destruction.
	void noticeTileChanges(int index, Tile *tile);
	/// Notes a tile's parts and fire on the first frame, so later frames can tell changes.
	void startWatchingTile(int index, Tile *tile);
	/// One tile's snapshot: models, animation, light, what has been seen.
	void snapshotTile(int index, Tile *tile, bool trackWalls);
	/// Notes the light sources and UFO machinery on a tile near the camera.
	void noteLightsAndDevices(const Tile *tile, const TileView &tileView);
	/// Keeps only the lights nearest the camera.
	void keepNearestLights();
	/// Advances the swinging doors and hands them to the rays.
	void updateDoorSwings(int tileCount);
	/// Finds the crafts and UFOs on the map, whose unseen sides are mirrored.
	void findMirrors();
	/// The colour of whatever is solid at a world voxel this frame (0 if nothing).
	Uint8 voxelColorAt(int worldX, int worldY, int worldZ, bool nextFrame = false) const;
	/// For a craft's west or north side: the colour of the side across from it.
	Uint8 mirroredColor(const Hit &hit, int axis, bool skinOnly = false, bool nextFrame = false) const;

	// =====================================================================================
	// Colours, light, the sky and the ray caster (FirstPersonViewRayCasting.cpp)
	// =====================================================================================

	SceneColors _colors = {};       // this frame's colours
	bool _smoothLighting = true;    // blend the game's light between tiles (an option)
	bool _flashlight = false;       // shoulder flashlight on (dark battles with personal lights on)
	float _flashlightPos[3] = {};   // where it is this frame
	float _muzzleFlashPos[3] = {};  // where the last shot flashed, lighting its surroundings for a moment
	Uint32 _muzzleFlashTick = 0;    // when it flashed
	float _muzzleFlashLight = 0.0f; // the flash's strength this frame

	/// Picks this frame's colours from the palette, for the time of day.
	void updateColors();
	/// The game's light at a point, blended between tile centres (0 bright .. 15 dark); also a UFO's light there.
	float lightAt(float x, float y, float z, float *ufoGlow = nullptr) const;
	/// Is there anything solid between two points? (A shadow ray.)
	bool shadowed(const float from[3], const float to[3]) const;
	/// Extra light at a point from nearby lamps and the flashlight, in shades.
	float pointLight(const float point[3], const float normal[3], bool shadows) const;
	/// The final colour of the voxel a ray hit: light, shading, glass, mirroring.
	Uint8 shadeHit(Hit &hit, const float rayOrigin[3], const float rayDirection[3], int pixelX, int pixelY) const;
	/// What a ray sees after leaving the map: the border tape, the ground, the sky.
	Uint8 colorOutsideMap(const float rayOrigin[3], const float rayDirection[3], float tExit, int exitAxis, int pixelX, int pixelY, float &depth) const;
	/// The sky's colour in a direction.
	Uint8 skyColor(const float rayDirection[3], int pixelX, int pixelY) const;
	/// The ray caster's helper threads. They are started on the first frame and then kept,
	/// waiting between frames, since starting threads anew every frame costs time. Each frame
	/// they and the drawing thread take the view's rows one at a time from a shared counter
	/// until none are left, so a thread on a fast core simply does more rows, and one on a slow
	/// core (a hybrid CPU's efficient cores) can't hold the frame up with a fixed share.
	struct RayWorkers
	{
		std::vector<std::thread> threads;  // the helpers (the drawing thread is the other one)
		std::mutex mutex;                  // guards frame, working and stop
		std::condition_variable wake;      // the helpers wait on it for the next frame
		std::condition_variable finished;  // the drawing thread waits on it for the helpers
		unsigned frame = 0;                // counts the frames handed out
		int working = 0;                   // helpers still casting rays for this frame
		bool stop = false;                 // the view is going away: the helpers end
		std::atomic<int> nextBlockRow{0};  // the next row (of pixel blocks) no thread has taken yet
		/// Stops the helpers and waits for them to end.
		~RayWorkers();
	};
	RayWorkers _rayWorkers; // the ray caster's threads (see castAllRays)

	/// Casts the rays for every pixel, on up to sixteen threads.
	void castAllRays();
	/// What each helper thread does: waits for a frame, casts rays for rows, and so on.
	void rayWorkerLoop();
	/// Casts the rays for rows taken one at a time from the shared counter, until none are left.
	void renderRows();

	// =====================================================================================
	// Units, items and markers as sprites in the world (FirstPersonViewSprites.cpp)
	// =====================================================================================

	struct Glide { double pos[3]; Uint32 seen; };
	std::unordered_map<const BattleUnit*, Glide> _unitGlides; // where each unit is drawn: eases after the game's steps
	Uint32 _unitLastTick = 0;                           // when the units' glides were last advanced
	struct UnitWatch { int health; bool out; Uint32 hitTick; float push[2]; };
	std::unordered_map<const BattleUnit*, UnitWatch> _watchedUnits; // hits and kills, for flinches and hitmarkers
	Uint32 _hitMarkTick = 0;                            // when an enemy in sight was last hit
	bool _hitMarkKill = false;                          // and whether that hit killed it
	std::vector<std::pair<const BattleUnit*, int>> _markers; // visible enemies and their numbers
	struct MarkerSpot { double pos[3]; int number; };
	std::vector<MarkerSpot> _markerSpots;               // where they are this frame
	int _uiScale = 1;                                   // markers drawn this many times bigger
	Surface *_spriteBuffer;                             // scratch surface for drawing a unit's sprite before it goes into the world
	std::vector<Surface*> _blendBuffers;                // sprites dissolving into their next frame, one per sprite size
	std::vector<int> _spriteColumns;                    // drawSprite: the sprite column under each screen column
	std::vector<int> _blendColumns;                     // drawSpriteBlend's source column and row lookups
	struct SpriteBox { int x0, y0, x1, y1; bool valid; }; // where a sprite frame's pixels are
	std::map<const Surface*, SpriteBox> _spriteBoxes;   // measured once per sprite frame

	/// Projects a point in voxels to the view; false if it's behind the camera.
	bool projectToScreen(const double world[3], double &screenX, double &screenY, double &depth) const;
	/// Draws a sprite standing in the world, depth-tested.
	void drawSprite(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade, double scale = 1.0);
	/// Draws a sprite part way through dissolving into the next animation frame (ordered dither).
	void drawSpriteBlend(const Surface *from, const Surface *to, double blend, int anchorX, int anchorY, const double world[3], double depthBias, int shade);
	/// The box around a sprite frame's pixels.
	const SpriteBox &spriteBox(const Surface *sprite);
	/// Watches the units for hits and kills (flinches, blood, hitmarkers).
	void watchUnits();
	/// Draws the visible units as their sprites, turned to face the camera.
	void drawUnits();
	/// Draws a soft shadow under a unit.
	void drawUnitShadow(const double feet[3], double radius);
	/// Darkens the ground in a soft disc.
	void drawGroundShade(const double centre[3], double radius, int steps, float density);
	/// Draws the top item lying on each discovered tile.
	void drawFloorItems();
	/// Draws smoke and fire on discovered tiles.
	void drawSmokeAndFire();
	/// Draws the numbered markers over the visible enemies.
	void drawMarkers();

	// =====================================================================================
	// Sprite upscaling (option): frames through xBRZ, back to the palette, optionally
	// sharpened; made on a worker thread (the original shows until the bigger one is ready),
	// cached by the frame's pixels, made unshaded so the light is applied afterwards
	// (FirstPersonViewUpscaling.cpp)
	// =====================================================================================

	struct Upscaled { Surface *surface; Uint32 used; };
	std::unordered_map<Uint64, Upscaled> _upscaledSprites; // the upscaled frames made so far, by their pixels
	struct UpscaleJob { Uint64 key; int w, h; bool detail; std::vector<Uint8> pixels; std::vector<SDL_Color> palette; Uint64 paletteHash; };
	std::thread _upscaleThread;                         // the worker thread (started on first use)
	std::mutex _upscaleMutex;                           // guards the job queue and the finished results
	std::condition_variable _upscaleWake;               // wakes the worker when there's a job (or it should stop)
	std::deque<UpscaleJob> _upscaleJobs;                // frames waiting to be upscaled
	std::unordered_map<Uint64, std::vector<Uint8>> _upscaleDone; // finished results, waiting to be made into surfaces
	std::unordered_set<Uint64> _upscaleQueued;          // frames queued or being made (so none is queued twice)
	bool _upscaleStop = false;                          // tells the worker to finish
	Uint64 _upscalePalette = 0;                         // palette the cache was made for
	Uint32 _upscaleClock = 0;                           // counts uses, so the least recently used frames can be dropped

	/// The upscaling worker thread: makes the bigger versions of queued sprite frames.
	void upscaleWorker();
	/// A sprite frame's upscaled version, or the original until it's ready.
	const Surface *upscaled(const Surface *sprite);
	/// Is a sprite at this point big enough on screen to be worth upscaling?
	bool upscaleHere(const double world[3], double scale) const;
	/// Draws a sprite, upscaled when it's close.
	void drawSpriteSmooth(const Surface *sprite, int anchorX, int anchorY, const double world[3], double depthBias, int shade);

	// =====================================================================================
	// Particles: explosions, fire, smoke, weather, UFO devices, glows (FirstPersonViewEffects.cpp)
	// =====================================================================================

	FirstPersonParticles _particles; // all the particles
	bool _particlesOn = true;        // particles switched on for this view (off for the small helmet-camera view)
	Uint32 _particleLastTick = 0;    // when the particles were last advanced
	std::string _weather;                 // the battle's weather: "rain", "snow", "dust" or none
	bool _weatherChosen = false;                       // looked up for this battle
	std::vector<std::pair<float, float>> _bubbleVents; // sea floor spots bubbles rise from
	std::vector<float> _glowBuffer, _glowScratch;      // quarter resolution glow, blurred into a bloom
	std::set<const Explosion*> _seenExplosions;        // explosions already seen (each spawns its particles once)

	/// Is there open sky above this point?
	bool openSky(float x, float y, float z) const;
	/// Under the sea: is this point in open water?
	bool inWater(float x, float y, float z) const;
	/// The colour of the terrain at a point, for debris.
	Uint8 debrisColorAt(const float pos[3]) const;
	/// Spawns particles for what happens in the battle: shots, explosions, fire.
	void spawnBattleEffects(float elapsedSeconds);
	/// Spawns fire, embers, smoke and sparks for new explosions.
	void spawnExplosionEffects();
	/// Spawns flames and smoke from burning and smoky tiles near the camera.
	void spawnTileFireAndSmoke(float elapsedSeconds);
	/// Spawns flames on burning units in sight.
	void spawnBurningUnitFlames(float elapsedSeconds);
	/// Spawns dust motes, fireflies, bubbles and weather around the camera.
	void spawnAmbient(float elapsedSeconds);
	/// Spawns energy and blinking lights around UFO machinery, and motes in lamplight.
	void spawnDevices(float elapsedSeconds);
	/// Bloom: a soft glow around whatever shines.
	void drawLightHalos();
	/// Advances the particles and spawns new ones.
	void updateParticles();
	/// Draws the particles, depth-tested.
	void drawParticles();

	// =====================================================================================
	// Shots in flight, beams, impacts and marks (FirstPersonViewShots.cpp)
	// =====================================================================================

	/// How a shot looks in flight, from its damage type.
	enum Trail { TRAIL_NONE, TRAIL_FIRE, TRAIL_SMOKE, TRAIL_BEAM, TRAIL_PLASMA, TRAIL_STUN, TRAIL_ACID };
	const Projectile *_lastProjectile = nullptr; // the shot in flight last frame
	float _lastShotHead[3] = {};                 // where it was drawn
	bool _lastProjectileThrown = false;          // it was a thrown item (no trail, no impact)
	Trail _shotTrail = TRAIL_NONE;               // how it looks in flight
	Uint8 _shotTrailColor = 0;                   // its colour
	float _shotTrailOrigin[3] = {};              // where its trail (and a laser's beam) starts
	Uint32 _ownShotTick = 0;                     // when our own last shot left the muzzle
	const RuleItem *_ownShotAmmo = nullptr;      // what it was (the clip may be gone by now)
	// our own shot is drawn leaving the muzzle in hand: the game's path shifted by this offset,
	// which shrinks to nothing at the impact (the game fires from the tile edge, by compass point)
	double _muzzleScreen[2] = {};                 // the weapon in hand's muzzle on the view
	bool _muzzleValid = false;                    // the weapon in hand's muzzle is on the view
	float _shotOffset[3] = {}, _shotFrom[3] = {}; // from the game's start of the path to the muzzle; that start
	float _shotLength = 1.0f;                     // the path's length, in voxels
	// dual wield: the other gun's round (Map::getPartnerProjectile), drawn the same way from the
	// other gun's muzzle
	double _offHandMuzzleScreen[2] = {};
	bool _offHandMuzzleValid = false;                   // the other gun's muzzle is on the view
	const Projectile *_lastPartnerShot = nullptr;       // the other gun's round in flight last frame
	float _partnerFrom[3] = {}, _partnerOffset[3] = {}, _partnerHead[3] = {}, _partnerOrigin[3] = {}; // its path's start, the offset to its muzzle, where it's drawn, where its trail starts
	float _partnerLength = 1.0f;                        // its path's length, in voxels
	Trail _partnerTrail = TRAIL_NONE;                   // how it looks in flight
	Uint8 _partnerColor = 0;                            // its colour
	Uint32 _partnerShotTick = 0;                        // when it left the muzzle
	const RuleItem *_partnerShotAmmo = nullptr;         // what it was (the clip may be gone by now)
	// under fire: shots whizzing past, where they come from
	Uint32 _nearMissTick = 0;
	double _nearMissDirection[2] = {}; // which way it passed (x, y in the world)
	bool _nearMissDone = true;         // this shot already whizzed past
	bool _shotIsEnemy = false;         // the shot in flight is an enemy's
	double _enemyShotFrom[3] = {};     // where it came from
	Uint32 _enemyShotEnd = 0;          // when it ended
	struct Beam { float from[3], to[3]; Uint32 start; Uint8 color; };
	std::vector<Beam> _beams;                          // laser beams fading out
	struct Decal { float pos[3]; Uint8 size, depth; }; // bullet holes and scorch marks on surfaces
	std::vector<Decal> _decals;                        // the marks left so far
	struct Scorch { float pos[3]; float radius; };     // blackened ground around explosions
	std::vector<Scorch> _scorches;                     // the blackened patches so far
	struct FrameClock { int frame; Uint32 since, length; };
	std::map<const Explosion*, FrameClock> _explosionClocks; // when each explosion last changed frame, and how long frames last
	// thrown items fly as voxel models of their inventory pictures (FirstPersonVoxelModel.h), one
	// per kind of item, made the first time one flies, tumbling end over end
	struct ItemModel { std::vector<VoxelFace> faces; float middle[2]; }; // middle: of the picture's opaque pixels
	std::unordered_map<const RuleItem*, ItemModel> _itemModels;
	Surface *_itemPicture = nullptr; // scratch surface an item's inventory picture is drawn on
	Uint32 _thrownStart = 0;         // when the thrown item in flight left the hand

	/// Follows the game's shot in flight: its start, flight and end.
	void followShots();
	/// The last shot has ended: impact, beam, when an enemy shot ended.
	void finishShot();
	/// A new shot has left: its start, look, muzzle flash and kick.
	void beginShot(Projectile *projectile);
	/// The shot moved on: its trail grows, and it may whizz past the eye.
	void advanceShot(Projectile *projectile);
	/// Draws the flying projectile (or thrown item) and the explosions.
	void drawProjectilesAndExplosions();
	/// The voxel model of an item's kind, made from its inventory picture the first time.
	const ItemModel &itemModel(BattleItem *item);
	/// Draws a thrown item in flight as its voxel model, tumbling; false if it has none.
	bool drawThrownItem(const Projectile *projectile, BattleItem *item);
	/// Fills a convex four-sided shape on the view at one depth, behind what's nearer.
	void fillQuadAtDepth(const double corners[4][2], Uint8 color, float depth);
	/// How a shot looks in flight, from its ammo's damage type.
	void trailFor(const BattleItem *ammo, Trail &trail, Uint8 &color) const;
	/// Spawns what a shot leaves behind along the bit of its path it flew this frame.
	void spawnTrailSegment(const float from[3], const float to[3]);
	/// Where the shot in flight is drawn: the game's position, our own shot's eased from the muzzle.
	void shotHead(const Projectile *projectile, float out[3]) const;
	/// Spawns sparks, debris and a mark where a shot ended.
	void spawnShotImpact(const float head[3], Trail trail);
	/// Draws the head of a shot in flight.
	void drawShotDot(const float head[3], Trail trail, Uint8 color);
	/// Dual wield: the other gun's round, from its own muzzle.
	void spawnPartnerEffects();
	/// Draws laser beams fading out.
	void drawBeams();
	/// Draws a glowing line in the world, depth-tested.
	void drawLine3D(const float from[3], const float to[3], Uint8 color, int thickness, float opacity);
	/// Leaves a bullet hole or scorch mark.
	void addDecal(const float pos[3], int size, int darkness);
	/// Draws the bullet holes, scorch marks and blackened ground.
	void drawDecals();
	/// Spawns chunks and dust from a destroyed tile part.
	void spawnDebris(const Position &tilePosition, int part, const MapData *destroyedMapData);

	// =====================================================================================
	// Tactical flares (FirstPersonViewFlares.cpp)
	// =====================================================================================

	struct Flare { float pos[3], vel[3]; float age; bool resting; };
	std::vector<Flare> _flares; // tactical flares, burning for a minute
	/// A flare's light in this frame: where its flame is, how far it has burnt down (fade) and
	/// its flicker. The same for every pixel of a frame, so it's worked out once a frame
	/// (prepareFlareLights) instead of once for every pixel the flare lights.
	struct FlareLight { float pos[3]; float fade, flicker; };
	std::vector<FlareLight> _flareLights; // this frame's light of each flare, in the order of _flares

	/// Flies, bounces and burns the tactical flares.
	void updateFlares(float elapsedSeconds);
	/// Works out each flare's light for this frame, before the rays use it.
	void prepareFlareLights();
	/// How much a point is lit by the flares (0..1).
	float flareLight(const float point[3], const float normal[3], bool shadows) const;
	/// Draws the flares' red-hot glow.
	void drawFlares();

	// =====================================================================================
	// Effects on the screen itself (FirstPersonViewScreenEffects.cpp, and the colour wash
	// at the end of draw)
	// =====================================================================================

	float _tintAmount = 0.0f;   // colour wash 0..1 (see _tintKind)
	int _tintKind = 0;          // which wash setTint asked for
	float _tintRedShare = 0.0f; // share of the washed pixels washed red instead (berserk's white, a zombie's green)
	float _vignette = 0.0f;     // red at the screen edges (badly hurt), 0..1
	float _staticAmount = 0.0f; // TV static
	Uint32 _staticSeed = 1;     // random seed for the static
	// on fire: a sheet of flame up from the bottom of the view (heat rising and cooling cell by
	// cell, in blocks of the 320x200 view's pixels) and embers drifting up out of it
	std::vector<Uint8> _flameHeat;
	int _flameGridWidth = 0, _flameGridHeight = 0; // the flame sheet's size, in cells
	bool _flamesLit = false;                       // some heat left (burning, or dying down)
	float _flameStepTimer = 0.0f;                  // time towards the next step of the flames
	Uint32 _flameSeed = 1;                         // random seed for the flames
	struct Ember { float x, y, vx, vy, age, life, phase; };
	std::vector<Ember> _embers;  // embers drifting up out of the flames
	float _burningAmount = 0.0f; // how fiercely (0 none .. 1)
	float _flameBurst = 0.0f;    // a burst of it, dying down
	float _emberDebt = 0.0f;     // the part of an ember still to spawn
	Uint32 _flameLastTick = 0;   // when the flames were last advanced
	// hit: where it came from, and blood on the view
	double _hurtDirection[2] = {};    // direction of the last hit, in the world
	Uint32 _hurtStart = 0;            // when the last hit's arc started
	bool _hurtDirectionKnown = false; // the last hit came from somewhere known
	struct Splat { float x, y, r; };
	std::vector<Splat> _bloodSplats; // blood drops on the view
	Uint32 _bloodStart = 0;          // when it splattered
	float _bloodAmount = 0.0f;       // how much (it fades)
	Uint32 _bloodSeed = 12345;       // random seed for the drops

	/// Washes the finished picture: night vision, colour wash, static, red edges.
	void applyScreenWash();

	// =====================================================================================
	// UFOs (an option, on by default): steady hulls whose lights glow and pulse gently, a
	// coloured pool of light around each UFO, and an aura of drifting motes
	// (FirstPersonViewUfo.cpp)
	// =====================================================================================
	/// A UFO part's steady 3D-only model, made from its original frames: the first frame's
	/// shape and colours, with the voxels that light up in any frame marked as lights (in their
	/// brightest colour), the hidden sides coloured from the hull only, and each voxel's open
	/// faces for smooth shading.
	struct UfoModel
	{
		VoxelModel model;           // what the rays see (its lit and exposed point at the arrays below)
		Uint16 lit[24][16];         // [z][y], bit x: a light
		Uint8 exposed[24][16][16];  // [z][y][x]: open faces, bit per face (+x -x +y -y +z -z)
	};
	/// One UFO on the map: its tiles, its size, and its light's colours.
	struct UfoSpot
	{
		std::vector<int> tiles;      // its tiles (indexes into the map)
		std::vector<int> lifts;      // its tiles with a grav lift floor, for their beams of light
		std::vector<float> liftBottoms; // for each lift: the height its beam starts at (the floor below it), in voxels
		float centre[2];             // the middle of its footprint, in world voxels (x, y)
		float half[2];               // half its footprint's size across x and y, in voxels
		float bottom, top;           // the heights of its lowest and highest tile's floor and roof, in voxels
		bool seen;                   // one of its tiles has been seen (or debug mode): its light and aura show
		bool colorsChosen;           // its light colours have been picked for the current palette
		Uint8 tint[2][256];          // each palette colour in its inner and its outer light colour
		Uint8 moteColor[3];          // the aura's colours: inner light, outer light, a white-hot spark
	};
	std::unordered_map<const MapData*, UfoModel> _ufoModels; // by MapData: UFO parts' steady models (built by the view that owns the models)
	std::vector<UfoSpot> _ufos;          // the UFOs on the map
	std::vector<Uint8> _ufoOfTile;       // per tile: which UFO's light reaches it (its index + 1), 0 none
	std::vector<Uint8> _ufoPhaseOfTile;  // per tile of a UFO: its lights' pulse phase (bits 0-3, by its angle round the UFO), UFO tile and outer skin marks
	std::vector<Uint8> _ufoGlowOfTile;   // per tile: how brightly its UFO's light falls there (0..255 at the tile's middle)
	bool _ufoLightsOn = false;           // the option is on and the map has UFOs (this frame)
	float _ufoPulse[2][16] = {};         // this frame's shade of a UFO light: [small light, big panel][phase]
	float _ufoBreath = 1.0f;             // this frame's strength of the UFOs' pools of light (they breathe slowly)
	float _ufoFaceShade[64] = {};        // this frame's face shade for each set of open faces (smooth hull shading), -1 none
	Uint32 _ufoPaletteHash = 0;          // the palette the UFOs' light colours were picked for
	/// Finds the UFOs on the map and builds their parts' steady models.
	void findUfos();
	/// Builds a UFO part's steady model from its original frames.
	void buildUfoModel(const MapData *mapData, const float outward[2], UfoModel &ufoModel);
	/// Picks a UFO's light colours from its lights, and makes its tint tables.
	void chooseUfoColors(UfoSpot &ufo);
	/// This frame's UFO lights: which UFOs have been seen, the pulse, the smooth shading.
	void updateUfos();
	/// Spawns the motes drifting around the UFOs in sight.
	void spawnUfoAura(float elapsedSeconds);
	/// Draws the flames rising up the view while the soldier burns.
	void drawBurning();
	/// Draws the arc towards where a hit came from, and blood on the view.
	void drawHurt();
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
