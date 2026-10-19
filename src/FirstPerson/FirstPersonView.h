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
#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <thread>
#include <unordered_set>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <vector>
#include "../Battlescape/Position.h"
#include "../Engine/InteractiveSurface.h"
#include "FirstPersonParticles.h"

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
 */
class FirstPersonView : public InteractiveSurface
{
public:
	/// Colour model of one MapData record: palette index per voxel, 0 = empty.
	struct VoxelModel
	{
		Uint8 color[24][16][16]; // [z][y][x]
		Uint16 glass[24][16];    // [z][y], bit x: window glass, seen through
		Uint8 min[3], max[3];    // inclusive bounds of solid voxels (x, y, z)
		bool empty;
		float glow[3];           // centre of the brightest voxels in the top third (a lamp's bulb), in the tile
		int glowCount;
		Uint8 glowColor;         // one of those bright colours
		Uint8 glowMinZ;          // only voxels from this height up shine
		Uint8 plan[16][16];      // [y][x] the colour seen from above: an underside shows this (the iso art never shows undersides)
		Uint8 ceiling;           // a thin plate's (floor, roof) underside: its most common top colour, 0 if not a plate
		Uint8 east[24][16];      // [z][y] the colour seen from the east: a west-facing side shows this (the iso art shows only east and south sides)
		Uint8 south[24][16];     // [z][x] the colour seen from the south: a north-facing side shows this
	};
	/// Per-frame snapshot of what a ray needs to know about a tile.
	struct TileView
	{
		const VoxelModel *part[4];
		const VoxelModel *next[4]; // an animated part's next frame, which it dissolves into
		Uint8 min[3], max[3];
		Uint8 light;             // the game's shade for the tile: 0 brightest .. 15 darkest
		bool discovered;         // the floor (and the object on it) has been seen
		Uint8 seen;              // bit per part seen: walls are seen from either side, like on the map
		bool empty;
		Sint16 swing[2];         // door swings reaching into this tile this frame, -1 none
		Uint8 emissive;          // bit per part: a light source, whose bright voxels shine
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
		int axis;                // axis of the face it entered through, -1 if unknown
		int tile;                // tile index
		int part;                // tile part hit, -1 for a swinging door
		Uint8 voxel[3];          // which voxel of the part (x, y, z in the tile)
		bool glass;              // the ray went through window glass on the way
		bool emissive;           // hit the bright part of a light source
	};
	/// Palette colours picked for the current palette and time of day.
	struct Look
	{
		Uint8 sky[32];           // horizon .. zenith
		Uint8 cloud[8];          // lit .. shadowed
		Uint8 haze, star, moon, sun, tapeYellow, tapeBlack, post;
		float night;             // 0 day .. 1 night
		bool underwater;
		int ambient;             // shade of open ground under the sky
		float faceShade[6];      // extra shade per face normal: +x -x +y -y +z -z
		float sunDir[3], moonDir[3];
		float cloudTime;
		Uint8 glassTint[256];    // each colour as seen through glass
		Uint32 paletteHash;      // palette the tint table was built for
		Uint8 fire[4];           // white-hot .. dark red
		Uint8 blood[2];          // splatter on the screen: dark, bright
		Uint8 smoke, spark, dust, rain, snow, sand, firefly, bubble;
		Uint8 marker, marine;
		Uint8 redTint[256];      // each colour washed with red (hurt, dying)
		Uint8 blueTint[256];     // ... with dark blue (passing out)
		Uint8 orangeTint[256];   // ... with orange (falling under mind control)
		Uint8 whiteTint[256];    // ... with white (a breakdown)
		Uint8 greenTint[256];    // ... with a sickly green (turning into a zombie)
		Uint8 grey[4];           // TV static
		Uint8 flareTint[256];    // each colour in a flare's red light
		Uint8 flareFire[3];      // flare flame: red, orange, yellow
	};
private:
	Game *_game;
	SavedBattleGame *_save;
	Map *_map;
	/// Sprite frames of a MapData record, to tell which animation frame a tile shows.
	struct Frames
	{
		int sprite[8];
		const Uint8 *buffer[8];
		bool animated;
	};
	/// A door that opened recently and is still swinging.
	struct DoorAnimation
	{
		int tile, part, hiddenPart;
		const VoxelModel *model;
		Uint32 start;
		bool away;               // swings into the neighbour tile, away from a camera on the door's own side
	};
	std::unordered_map<Uint64, VoxelModel> _models;     // by MapData, sprite and cut-out
	std::unordered_map<const MapData*, Frames> _frames;
	std::vector<const MapData*> _lastWalls[2];          // west/north wall of each tile last frame
	std::vector<DoorAnimation> _doorAnimations;
	std::unordered_map<int, Uint32> _ufoDoorOpening;    // UFO door part (tile * 4 + part) -> when it started sliding
	std::vector<DoorSwing> _swings;                     // this frame
	std::vector<std::pair<const BattleUnit*, int>> _markers; // visible enemies and their numbers
	struct MarkerSpot { double pos[3]; int number; };
	std::vector<MarkerSpot> _markerSpots;               // where they are this frame
	struct Glide { double pos[3]; Uint32 seen; };
	std::unordered_map<const BattleUnit*, Glide> _glides; // where each unit is drawn: eases after the game's steps
	Uint32 _unitTick = 0;
	struct LightSpot { float pos[3]; float strength; Uint8 color; };
	std::vector<LightSpot> _lights;                     // light sources near the camera this frame
	struct DeviceSpot { float pos[3]; float half[3]; bool power; };
	std::vector<DeviceSpot> _devices;                   // UFO power sources and navigation consoles nearby
	std::vector<std::pair<float, float>> _vents;        // sea floor spots bubbles rise from
	const VoxelModel *_ground;  // floor repeated outside the map
	bool _groundChosen;
	std::vector<TileView> _tileViews;
	std::vector<float> _depth;
	std::vector<Uint8> _glowMask;       // pixels showing something that shines (lit voxels, sparks)
	std::vector<float> _glowBuf, _glowTmp; // quarter resolution glow, blurred into a bloom
	double _eye[3];
	double _yaw, _pitch;        // radians; yaw 0 = +x (east), pi/2 = +y (south)
	double _fwd[3], _right[3], _up[3]; // camera axes, in true proportions (see ZScale)
	double _fwdVoxel[3] = { 1.0, 0.0, 0.0 }; // the view direction in the voxel grid
	double _fovTan;             // tan(horizontal fov / 2)
	float _maxDistance;         // in voxels
	bool _smoothLighting;
	FirstPersonView *_cacheOwner; // another view whose voxel models this one uses, or null
	bool _particlesOn;
	float _tint;                // colour wash 0..1 (see _tintColor)
	bool _flashlight = false;   // shoulder flashlight on (dark battles with personal lights on)
	float _flash[3] = {};       // where it is this frame
	float _shake = 0.0f;        // camera shake, voxels
	float _vignette = 0.0f;     // red at the screen edges (badly hurt), 0..1
	float _kick = 0.0f;         // recoil: the view kicked up after your own shot, degrees
	int _uiScale = 1;           // markers drawn this many times bigger
	double _muzzleScreen[2] = {}; // the weapon in hand's muzzle on the view
	bool _muzzleValid = false;
	int _hiddenFloor = -1;      // a floor left out: the lift platform the camera rides through
	int _tintColor = 0;         // which wash setTint asked for
	float _tintRed = 0.0f;      // share of the washed pixels washed red instead (berserk's white, a zombie's green)
	// on fire: a sheet of flame up from the bottom of the view (heat rising and cooling cell by
	// cell, in blocks of the 320x200 view's pixels) and embers drifting up out of it
	std::vector<Uint8> _fireHeat;
	int _fireW = 0, _fireH = 0;
	bool _fireLit = false;      // some heat left (burning, or dying down)
	float _fireStep = 0.0f;     // time towards the next step of the flames
	Uint32 _fireSeed = 1;
	struct Ember { float x, y, vx, vy, age, life, phase; };
	std::vector<Ember> _embers;
	float _onFire = 0.0f;       // how fiercely (0 none .. 1)
	float _flareUp = 0.0f;      // a burst of it, dying down
	float _emberSpawn = 0.0f;   // the part of an ember still to spawn
	Uint32 _flameTick = 0;
	void drawBurning();
	// under fire: shots whizzing past, where hits come from, blood on the view
	Uint32 _nearMissTick = 0;
	double _nearMissDir[2] = {};
	bool _nearMissDone = true;  // this shot already whizzed past
	bool _shotIsEnemy = false;
	double _enemyShotFrom[3] = {};
	Uint32 _enemyShotEnd = 0;
	double _hurtFrom[2] = {};   // direction of the last hit, in the world
	Uint32 _hurtStart = 0;
	bool _hurtFromKnown = false;
	struct Splat { float x, y, r; };
	std::vector<Splat> _splats;
	Uint32 _splatStart = 0;
	float _splatAmount = 0.0f;
	Uint32 _splatSeed = 12345;
	void drawHurt();
	float _static = 0.0f;       // TV static
	Uint32 _staticSeed = 1;
	// animated tiles step a frame every game animation tick: how far into the tick we are
	int _lastAnimFrame = -1;
	Uint32 _animChanged = 0, _animPeriod = 100;
	float _animBlend = 0.0f;
	Uint32 _ownShotTick = 0;    // when our own last shot left the muzzle
	const RuleItem *_ownShotAmmo = nullptr; // what it was (the clip may be gone by now)
	float _muzzle[3] = {};      // where the last shot flashed, lighting its surroundings for a moment
	Uint32 _muzzleTick = 0;
	float _muzzleLight = 0.0f;  // the flash's strength this frame
	struct UnitWatch { int health; bool out; Uint32 hitTick; float push[2]; };
	std::unordered_map<const BattleUnit*, UnitWatch> _watch; // hits and kills, for flinches and hitmarkers
	Uint32 _hitMarkTick = 0;
	bool _hitMarkKill = false;
	struct Decal { float pos[3]; Uint8 size, depth; };   // bullet holes and scorch marks on surfaces
	std::vector<Decal> _decals;
	struct Scorch { float pos[3]; float radius; };      // blackened ground around explosions
	std::vector<Scorch> _scorches;
	std::vector<const MapData*> _lastParts[4];          // every tile part last frame, to notice destruction
	std::vector<Uint8> _burning;                        // tiles on fire last frame: burnt out, they leave a scorch
	Uint32 _shakeTick = 0;
	int _nightVision;           // night vision colour group + 1, 0 off
	const BattleUnit *_cameraUnit;
	Surface *_spriteBuffer;
	std::vector<Surface*> _blendBuffers; // sprites dissolving into their next frame, one per sprite size
	std::vector<int> _spriteColumns;     // drawSprite: the sprite column under each screen column
	// sprite upscaling (option): frames through xBRZ, back to the palette, optionally sharpened;
	// made on a worker thread (the original shows until the bigger one is ready), cached by the
	// frame's pixels, made unshaded so the light is applied afterwards
	struct Upscaled { Surface *surface; Uint32 used; };
	std::unordered_map<Uint64, Upscaled> _upscaled;
	struct UpscaleJob { Uint64 key; int w, h; bool detail; std::vector<Uint8> pixels; std::vector<SDL_Color> palette; Uint64 paletteHash; };
	std::thread _upscaleThread;
	std::mutex _upscaleMutex;
	std::condition_variable _upscaleWake;
	std::deque<UpscaleJob> _upscaleJobs;
	std::unordered_map<Uint64, std::vector<Uint8>> _upscaleDone;
	std::unordered_set<Uint64> _upscaleQueued;
	bool _upscaleStop = false;
	Uint64 _upscalePalette = 0;          // palette the cache was made for
	Uint32 _upscaleClock = 0;
	void upscaleWorker();
	const Surface *upscaled(const Surface *src);
	std::vector<int> _blendColumns;     // drawSpriteBlend's source column and row lookups
	bool upscaleHere(const double world[3], double scale) const;
	void drawSpriteSmooth(const Surface *src, int anchorX, int anchorY, const double world[3], double depthBias, int shade);
	struct SpriteBox { int x0, y0, x1, y1; bool valid; }; // where a sprite frame's pixels are
	std::map<const Surface*, SpriteBox> _spriteBoxes;
	const SpriteBox &spriteBox(const Surface *sprite);
	struct FrameClock { int frame; Uint32 since, length; };
	std::map<const Explosion*, FrameClock> _explosionClocks; // when each explosion last changed frame, and how long frames last
	Look _look;
	FirstPersonParticles _particles;
	const Projectile *_lastProjectile;
	float _lastHead[3];
	bool _lastProjectileThrown;
	/// How a shot looks in flight, from its damage type.
	enum Trail { TRAIL_NONE, TRAIL_FIRE, TRAIL_SMOKE, TRAIL_BEAM, TRAIL_PLASMA, TRAIL_STUN, TRAIL_ACID };
	Trail _trail = TRAIL_NONE;
	Uint8 _trailColor = 0;
	float _trailOrigin[3] = {};
	// our own shot is drawn leaving the muzzle in hand: the game's path shifted by this offset,
	// which shrinks to nothing at the impact (the game fires from the tile edge, by compass point)
	float _shotOffset[3] = {}, _shotFrom[3] = {};
	float _shotLength = 1.0f;
	// dual wield: the other gun's round (Map::getPartnerProjectile), drawn the same way from the
	// other gun's muzzle
	double _offMuzzleScreen[2] = {};
	bool _offMuzzleValid = false;
	const Projectile *_lastPartner = nullptr;
	float _partnerFrom[3] = {}, _partnerOffset[3] = {}, _partnerHead[3] = {}, _partnerOrigin[3] = {};
	float _partnerLength = 1.0f;
	Trail _partnerTrail = TRAIL_NONE;
	Uint8 _partnerColor = 0;
	Uint32 _partnerShotTick = 0;
	void spawnPartnerEffects();
	void trailFor(const BattleItem *ammo, Trail &trail, Uint8 &color) const;
	void shotLanded(const float head[3], Trail trail);
	void drawShotDot(const float head[3], Trail trail, Uint8 color);
	struct Beam { float from[3], to[3]; Uint32 start; Uint8 color; };
	std::vector<Beam> _beams;                           // laser beams fading out
	struct Flare { float pos[3], vel[3]; float age; bool resting; };
	std::vector<Flare> _flares;                         // tactical flares, burning for a minute
	std::set<const Explosion*> _seenExplosions;
	Uint32 _particleTick;
	std::string _weather;
	bool _weatherChosen;

	const VoxelModel *getModel(const MapData *md, int sprite = -1, bool cutout = false);
	void buildModel(const MapData *md, int sprite, bool cutout, VoxelModel &model) const;
	const Frames &getFrames(const MapData *md);
	void trackDoors(int index, Tile *tile);
	void chooseGround();
	void snapshotTiles();
	void updateBasis();
	void updateLook();
	float lightAt(float x, float y, float z) const;
	Uint8 shadeHit(const Hit &hit, const float o[3], const float d[3], int px, int py) const;
	Uint8 outsideColor(const float o[3], const float d[3], float tExit, int exitAxis, int px, int py, float &depth) const;
	Uint8 skyColor(const float d[3], int px, int py) const;
	void renderRows(int first, int step);
	bool project(const double world[3], double &sx, double &sy, double &depth) const;
	void drawSprite(const Surface *src, int anchorX, int anchorY, const double world[3], double depthBias, int shade, double scale = 1.0);
	/// Draws a sprite part way through dissolving into the next animation frame (ordered dither).
	void preloadModels();
	// crafts and UFOs are symmetric: a side the iso art never shows (facing west or north) takes
	// its colours from the mirrored side it does show
	struct Mirror
	{
		int lo[3], size[3];          // the craft's voxels: first world voxel and extent per axis
		int sum[2];                  // mirror line across x and across y (twice its world coordinate)
		bool symmetric[2];           // the craft's shape mirrors across x / across y
		std::vector<int> start[2];   // per voxel row along x / y: where its surfaces begin in pos
		std::vector<Sint16> pos[2];  // voxels on those rows whose east / south face is open, in order
		std::vector<Sint16> outer[2];// per row: the craft's first voxel from the west / north, -1 none
	};
	std::vector<Mirror> _mirrors;
	std::vector<Sint16> _mirrorOf;   // per tile: its craft in _mirrors, -1 none
	void findMirrors();
	Uint8 colorAt(int wx, int wy, int wz) const;
	Uint8 mirroredColor(const Hit &hit, int axis, bool skinOnly = false) const;
	void drawSpriteBlend(const Surface *from, const Surface *to, double blend, int anchorX, int anchorY, const double world[3], double depthBias, int shade);
	void drawUnits();
	void drawFloorItems();
	void drawSmokeAndFire();
	void drawEffects();
	void updateParticles();
	void spawnEffects(float dt);
	void spawnAmbient(float dt);
	bool openSky(float x, float y, float z) const;
	bool inWater(float x, float y, float z) const;
	Uint8 colorAt(const float pos[3]) const;
	void drawParticles();
	void drawDecals();
	void drawGroundShade(const double centre[3], double radius, int steps, float density);
	void addDecal(const float pos[3], int size, int depth);
	void debris(const Position &tile, int part, const MapData *before);
	void watchUnits();
	void updateFlares(float dt);
	float flareLight(const float p[3], const float n[3], bool shadows) const;
	void drawFlares();
	void trailSegment(const float from[3], const float to[3]);
	/// Where the shot in flight is drawn: the game's position, our own shot's eased from the muzzle.
	void shotHead(const Projectile *projectile, float out[3]) const;
	void drawBeams();
	void drawLine3D(const float a[3], const float b[3], Uint8 color, int thickness, float opacity);
	bool shadowed(const float from[3], const float to[3]) const;
	float pointLight(const float p[3], const float n[3], bool shadows) const;
	void drawUnitShadow(const double feet[3], double radius);
	void spawnDevices(float dt);
	void drawHalos();
	void drawMarkers();
public:
	/// Creates the first-person view.
	FirstPersonView(Game *game, int width, int height, int x, int y);
	/// Cleans up the first-person view.
	~FirstPersonView();
	/// Sets the battle to render.
	void setSavedBattleGame(SavedBattleGame *save);
	/// Sets the isometric map, for projectiles and explosions.
	void setMap(Map *map) { _map = map; }
	/// Places the eye, in voxels.
	void setEye(double x, double y, double z);
	/// Gets the eye, in voxels.
	void getEye(double out[3]) const;
	/// Sets the unit being looked through (not drawn).
	void setCameraUnit(const BattleUnit *unit) { _cameraUnit = unit; }
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
	void getRayDirection(int x, int y, double out[3]);
	/// Sets the horizontal field of view.
	void setFieldOfView(double degrees);
	/// Sets how far the view reaches, in tiles.
	void setMaxDistance(int tiles);
	/// Blends the game's per-tile light smoothly instead of tile by tile.
	void setSmoothLighting(bool smooth) { _smoothLighting = smooth; }
	/// Marks units (the visible enemies) with their numbers, over everything.
	void setMarkers(const std::vector<std::pair<const BattleUnit*, int>> &markers) { _markers = markers; }
	/// Is the door in this wall still moving: a door swinging open, or a UFO door sliding?
	bool isDoorMoving(const Position &tile, int part);
	/// Uses another view's voxel models instead of building its own (a small extra view).
	void shareCache(FirstPersonView *owner) { _cacheOwner = owner; }
	/// Turns particles (sparks, smoke, weather...) on or off for this view.
	void setParticles(bool on) { _particlesOn = on; }
	/// Throws a tactical flare from the soldier's shoulder towards a point (a short toss under the sea).
	void throwFlare(const float target[3]);
	/// Turns the shoulder flashlight on or off.
	void setFlashlight(bool on) { _flashlight = on; }
	/// Where the weapon in hand's muzzle is on the view (pixels): our own shots flash and start their trails there.
	void setMuzzlePoint(double x, double y, bool valid) { _muzzleScreen[0] = x; _muzzleScreen[1] = y; _muzzleValid = valid; }
	void setOffMuzzlePoint(double x, double y, bool valid) { _offMuzzleScreen[0] = x; _offMuzzleScreen[1] = y; _offMuzzleValid = valid; } // dual wield: the other gun's
	Uint32 getPartnerShotTick() const { return _partnerShotTick; }
	/// The soldier was hurt: an arc on the screen's edge towards where it came from (a world
	/// point, or none), and with a heavy or close hit blood splattered over the view.
	void showHurt(const double *from, float amount, bool splatter);
	/// When an enemy shot last whizzed past close by (0 never), and from which side.
	Uint32 getNearMissTick() const { return _nearMissTick; }
	void getNearMissDirection(double out[2]) const { out[0] = _nearMissDir[0]; out[1] = _nearMissDir[1]; }
	/// Where the last enemy shot came from and when it ended (0 none yet).
	Uint32 getEnemyShotEnd(double from[3]) const { std::copy(_enemyShotFrom, _enemyShotFrom + 3, from); return _enemyShotEnd; }
	/// Leaves out one tile's floor (the lift platform being ridden through), or none (index -1).
	void setHiddenFloor(int tileIndex) { _hiddenFloor = tileIndex; }
	/// When our own last shot left the muzzle (for the weapon in hand's flash).
	Uint32 getOwnShotTick() const { return _ownShotTick; }
	/// The ammo our own last shot fired (null if unknown).
	const RuleItem *getOwnShotAmmo() const { return _ownShotAmmo; }
	/// Draws the enemy markers this many times bigger (big screens).
	void setUiScale(int scale) { _uiScale = scale; }
	/// The stereo angle of a sound from a tile, for this camera as the listener (0 ahead, 90 right).
	int soundAngle(const Position &tile) const;
	/// The game's shade (0 bright .. 15 dark) where the camera is.
	int getEyeShade() const;
	/// How far the view is kicked up by recoil right now, in degrees.
	float getKick() const { return _kick; }
	/// When an enemy in sight was last hit, and whether that hit killed it (for the hitmarker).
	Uint32 getHitMarkTick() const { return _hitMarkTick; }
	bool getHitMarkKill() const { return _hitMarkKill; }
	/// Reddens the screen edges (0 none .. 1 strong), for a soldier badly hurt.
	void setVignette(float amount) { _vignette = amount; }
	/// Shakes the camera (explosions, hits); stronger shakes win.
	void addShake(float amount) { _shake = std::max(_shake, amount); }
	/// Washes the view with a colour (0 none .. 1 full): 0 red (hurt, killed), 1 dark blue (unconscious),
	/// 2 orange (mind control), 3 white (a breakdown), 4 sickly green (turning into a zombie); `red`
	/// is the share of the washed pixels washed red instead.
	void setTint(float amount, int color = 0, float red = 0.0f) { _tint = amount; _tintColor = color; _tintRed = red; }
	/// The soldier is on fire: flames lick up from the bottom of the view (0 none .. 1 fierce).
	void setBurning(float amount) { _onFire = amount; }
	/// A burst of flames (catching fire), 0..1.
	void flareUp(float amount) { _flareUp = std::max(_flareUp, amount); }
	/// TV static over the view (a helmet camera losing its signal), 0 none .. 1 all.
	void setStatic(float amount) { _static = amount; }
	/// Night vision like the isometric map's: brighter, in one colour (group + 1, 0 off).
	void setNightVision(int color) { _nightVision = color; }
	/// Renders the view.
	void draw() override;
};

}
