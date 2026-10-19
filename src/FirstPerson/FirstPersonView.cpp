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
#include "FirstPersonView.h"
#include "../Engine/Scalers/xbrz.h"
#include "../Engine/Logger.h"
#include "../Engine/Sound.h"
#include <bitset>
#include "RuleFirstPerson.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <climits>
#include <array>
#include <set>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <chrono>
#include <SDL.h>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleDamageType.h"
#include "../Mod/RuleTerrain.h"
#include "../Mod/RuleUfo.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Battlescape/Explosion.h"
#include "../Battlescape/ItemSprite.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Position.h"
#include "../Battlescape/Projectile.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitSprite.h"

namespace OpenXcom
{

namespace
{

const double Pi = 3.14159265358979323846;
const float CellSize[3] = { (float)Position::TileXY, (float)Position::TileXY, (float)Position::TileZ };
const float Infinity = std::numeric_limits<float>::infinity();

/// Unit and item sprites are 32x40 isometric frames; this buffer fits a 2x2 unit.
const int SpriteBufferSize = 64;
/// Sprite upscaling: how much bigger (xBRZ), and from how many screen pixels per sprite pixel on.
const int UpscaleFactor = 4;
const double UpscaleFrom = 1.6;

/// How long doors take to open in the view, so you see them move (the game's own are quicker).
const Uint32 DoorSwingTime = 500, UfoDoorTime = 600;
/// Tactical flares: how long they burn, how many at once, how far their light reaches.
const float FlareLife = 60.0f, FlareReach = 150.0f;
const size_t FlareMax = 8;

/// Top of the ground-level floor (floors are solid in voxel layers 0-1).
const float GroundTop = 2.0f;
// X-COM's voxels aren't cubes. The isometric art is drawn in 2:1 dimetric with one pixel per voxel
// up, where a true projection of cubes would draw 1.22 pixels: one voxel up is sqrt(2/3) of one
// voxel across. The camera works in true proportions; rays walk the voxel grid.
const double ZScale = 0.81649658092772603;
const float ZScaleF = (float)ZScale;
// and an isometric sprite pixel across is 1/sqrt(2) of a voxel (32 pixels span a tile's diagonal)
const double SpritePixelX = 0.70710678118654752;

/// Ordered dithering, used to turn fractional shades and gradients into 8-bit colour.
const float Bayer[4][4] = {
	{ 0 / 16.0f, 8 / 16.0f, 2 / 16.0f, 10 / 16.0f },
	{ 12 / 16.0f, 4 / 16.0f, 14 / 16.0f, 6 / 16.0f },
	{ 3 / 16.0f, 11 / 16.0f, 1 / 16.0f, 9 / 16.0f },
	{ 15 / 16.0f, 7 / 16.0f, 13 / 16.0f, 5 / 16.0f } };

inline float dither(int x, int y) { return Bayer[y & 3][x & 3]; }

/**
 * Darkens a palette colour within its 16-colour group, like StandardShade:
 * anything that would spill into the next group becomes black.
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

inline float smoothstep(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

inline Uint32 hash2(int x, int y)
{
	Uint32 h = (Uint32)x * 374761393u + (Uint32)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/// Smooth value noise in [0,1).
float valueNoise(float x, float y)
{
	const int xi = (int)std::floor(x), yi = (int)std::floor(y);
	const float fx = x - xi, fy = y - yi;
	const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
	auto v = [](int a, int b) { return (hash2(a, b) & 0xFFFF) / 65536.0f; };
	const float a = v(xi, yi), b = v(xi + 1, yi), c = v(xi, yi + 1), d = v(xi + 1, yi + 1);
	return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

float cloudNoise(float x, float y)
{
	return valueNoise(x, y) * 0.55f + valueNoise(x * 2.1f + 17.0f, y * 2.1f) * 0.3f + valueNoise(x * 4.3f, y * 4.3f + 31.0f) * 0.15f;
}

/// 3x5 glyphs for the border tape text, rows top to bottom, bit 2 = left column.
const Uint8 TapeFont[][5] = {
	{ 5, 5, 2, 5, 5 }, // X
	{ 0, 0, 7, 0, 0 }, // -
	{ 7, 4, 4, 4, 7 }, // C
	{ 7, 5, 5, 5, 7 }, // O
	{ 5, 7, 7, 5, 5 }, // M
	{ 0, 0, 0, 0, 0 }, // space
};
const int TapeText[] = { 0, 1, 2, 3, 4, 5, 5 }; // "X-COM  "
/// 3x5 digits for the enemy markers, same layout.
const Uint8 DigitFont[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 3, 1, 7 }, { 5, 5, 7, 1, 1 },
	{ 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 } };
const int TapeTextLength = sizeof(TapeText) / sizeof(TapeText[0]);

struct RayContext
{
	const FirstPersonView::TileView *tiles;
	const FirstPersonView::DoorSwing *swings;
	int size[3];
	float maxDistance;
};

/**
 * Slab test along one axis; narrows [ta, tb] and remembers which face the ray came in through.
 */
inline bool slab(float o, float d, float lo, float hi, float &ta, float &tb, int axis, int &enterAxis)
{
	if (d == 0.0f)
	{
		return o >= lo && o < hi;
	}
	float tn = (lo - o) / d, tf = (hi - o) / d;
	if (tn > tf) std::swap(tn, tf);
	if (tn > ta) { ta = tn; enterAxis = axis; }
	if (tf < tb) tb = tf;
	return ta <= tb;
}

/**
 * A swinging door: a 16 x 2 x height slab turned around its hinge, coloured from the closed door.
 */
bool traceSwing(const FirstPersonView::DoorSwing &s, const float o[3], const float d[3], float t0, float t1, FirstPersonView::Hit &hit)
{
	const float ux = s.dir[0], uy = s.dir[1], vx = -uy, vy = ux;
	const float rx = o[0] - s.hinge[0], ry = o[1] - s.hinge[1];
	const float ou = rx * ux + ry * uy, ov = rx * vx + ry * vy;
	const float du = d[0] * ux + d[1] * uy, dv = d[0] * vx + d[1] * vy;
	float ta = t0, tb = t1;
	int face = -1;
	if (!slab(ou, du, -1.0f, 15.0f, ta, tb, 0, face) || !slab(ov, dv, -1.0f, 1.0f, ta, tb, 1, face) || !slab(o[2], d[2], s.z0, s.z1, ta, tb, 2, face))
	{
		return false;
	}
	const int u = std::clamp((int)(ou + du * ta) + 1, 0, 15);
	const int z = std::clamp((int)(o[2] + d[2] * ta - s.z0) + s.model->min[2], 0, 23);
	Uint8 color = 0;
	for (int k = 0; k < 2 && !color; ++k)
	{
		color = s.westWall ? s.model->color[z][u][k] : s.model->color[z][k][u];
	}
	if (!color)
	{
		return false;
	}
	hit.color = color;
	hit.t = ta;
	hit.part = -1;
	// shade by the world axis closest to the face the ray hit
	if (face == 2) hit.axis = 2;
	else if (face == 1) hit.axis = std::abs(vx) > std::abs(vy) ? 0 : 1;
	else hit.axis = std::abs(ux) > std::abs(uy) ? 0 : 1;
	return true;
}

enum RayEnd { RAY_HIT, RAY_SIDE, RAY_TOP, RAY_BOTTOM, RAY_FAR };

/**
 * Walks the voxels of one tile between t0 and t1, looking for the first solid one.
 * @param enterAxis Axis of the face the ray crossed to enter the tile, -1 at the eye.
 */
bool traceTile(const FirstPersonView::TileView &tv, const FirstPersonView::DoorSwing *swings, const int cell[3], const float o[3], const float d[3],
	float t0, float t1, int enterAxis, FirstPersonView::Hit &hit)
{
	bool doorHit = false;
	for (Sint16 swing : tv.swing)
	{
		if (swing >= 0 && traceSwing(swings[swing], o, d, t0, t1, hit))
		{
			doorHit = true;
			t1 = hit.t;
		}
	}
	if (tv.max[0] < tv.min[0])
	{
		return doorHit; // nothing else in the tile
	}
	// clip the segment to the bounding box of the solid voxels in this tile
	int lo[3], hi[3];
	float ta = t0, tb = t1;
	int axis = enterAxis;
	for (int a = 0; a < 3; ++a)
	{
		const int base = cell[a] * (int)CellSize[a];
		lo[a] = base + tv.min[a];
		hi[a] = base + tv.max[a];
		if (d[a] != 0.0f)
		{
			const float inv = 1.0f / d[a];
			float tn = (lo[a] - o[a]) * inv;
			float tf = (hi[a] + 1 - o[a]) * inv;
			if (tn > tf) std::swap(tn, tf);
			if (tn > ta) { ta = tn; axis = a; }
			if (tf < tb) tb = tf;
		}
		else if (o[a] < lo[a] || o[a] >= hi[a] + 1)
		{
			return false;
		}
	}
	if (ta > tb)
	{
		return doorHit;
	}

	int v[3], step[3];
	float tMax[3], tDelta[3];
	for (int a = 0; a < 3; ++a)
	{
		const float p = o[a] + d[a] * (ta + 1e-3f);
		v[a] = std::clamp((int)std::floor(p), lo[a], hi[a]);
		if (d[a] > 0.0f)
		{
			step[a] = 1;
			tDelta[a] = 1.0f / d[a];
			tMax[a] = (v[a] + 1 - o[a]) / d[a];
		}
		else if (d[a] < 0.0f)
		{
			step[a] = -1;
			tDelta[a] = -1.0f / d[a];
			tMax[a] = (v[a] - o[a]) / d[a];
		}
		else
		{
			step[a] = 0;
			tDelta[a] = Infinity;
			tMax[a] = Infinity;
		}
	}

	float t = ta;
	while (true)
	{
		const int lx = v[0] - cell[0] * Position::TileXY;
		const int ly = v[1] - cell[1] * Position::TileXY;
		const int lz = v[2] - cell[2] * Position::TileZ;
		for (int part = 0; part < 4; ++part)
		{
			const FirstPersonView::VoxelModel *m = tv.part[part];
			if (!m) continue;
			const Uint8 color = m->color[lz][ly][lx];
			if (!color) continue;
			if (m->glass[lz][ly] & (1 << lx))
			{
				hit.glass = true; // window glass: tint what's behind it
				continue;
			}
			hit.color = color;
			hit.t = t;
			hit.axis = axis;
			hit.part = part;
			hit.voxel[0] = (Uint8)lx;
			hit.voxel[1] = (Uint8)ly;
			hit.voxel[2] = (Uint8)lz;
			hit.emissive = ((tv.emissive >> part) & 1) && (color & 15) <= 3 && lz >= m->glowMinZ;
			return true;
		}
		const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		t = tMax[a];
		if (t > tb) return doorHit;
		v[a] += step[a];
		if (v[a] < lo[a] || v[a] > hi[a]) return doorHit;
		tMax[a] += tDelta[a];
		axis = a;
	}
}

/**
 * Two-level ray walk: tile by tile, and voxel by voxel inside tiles that have anything in them.
 * @return How the ray ended; for RAY_HIT the hit is filled in, otherwise tEnd/endAxis say where it left the map.
 */
RayEnd castRay(const RayContext &ctx, const float o[3], const float d[3], FirstPersonView::Hit &hit, float &tEnd, int &endAxis)
{
	hit.glass = false;
	hit.emissive = false;
	int cell[3], step[3];
	float tMax[3], tDelta[3];
	for (int a = 0; a < 3; ++a)
	{
		const float p = o[a] / CellSize[a];
		cell[a] = (int)std::floor(p);
		if (cell[a] < 0 || cell[a] >= ctx.size[a])
		{
			tEnd = 0.0f;
			endAxis = a;
			return a == 2 ? (cell[a] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		const float dt = d[a] / CellSize[a];
		if (dt > 0.0f)
		{
			step[a] = 1;
			tDelta[a] = 1.0f / dt;
			tMax[a] = (cell[a] + 1 - p) / dt;
		}
		else if (dt < 0.0f)
		{
			step[a] = -1;
			tDelta[a] = -1.0f / dt;
			tMax[a] = (p - cell[a]) / -dt;
		}
		else
		{
			step[a] = 0;
			tDelta[a] = Infinity;
			tMax[a] = Infinity;
		}
	}

	float t = 0.0f;
	int enterAxis = -1;
	while (t < ctx.maxDistance)
	{
		const int index = (cell[2] * ctx.size[1] + cell[1]) * ctx.size[0] + cell[0];
		const auto &tv = ctx.tiles[index];
		const float tExit = std::min(tMax[0], std::min(tMax[1], tMax[2]));
		if (!tv.empty && traceTile(tv, ctx.swings, cell, o, d, t, tExit, enterAxis, hit))
		{
			hit.tile = index;
			return RAY_HIT;
		}
		const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		t = tMax[a];
		cell[a] += step[a];
		if (cell[a] < 0 || cell[a] >= ctx.size[a])
		{
			tEnd = t;
			endAxis = a;
			return a == 2 ? (step[a] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		tMax[a] += tDelta[a];
		enterAxis = a;
	}
	tEnd = t;
	endAxis = -1;
	return RAY_FAR;
}

}

/**
 * Creates the first-person view.
 * @param game Pointer to the core game.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 */
FirstPersonView::FirstPersonView(Game *game, int width, int height, int x, int y) :
	InteractiveSurface(width, height, x, y), _game(game), _save(nullptr), _map(nullptr),
	_ground(nullptr), _groundChosen(false),
	_yaw(0.0), _pitch(0.0), _fovTan(std::tan(75.0 * Pi / 360.0)), _maxDistance(60.0f * Position::TileXY),
	_smoothLighting(true), _cacheOwner(nullptr), _particlesOn(true), _tint(0.0f), _nightVision(0), _cameraUnit(nullptr), _spriteBuffer(new Surface(SpriteBufferSize, SpriteBufferSize)), _look(),
	_lastProjectile(nullptr), _lastProjectileThrown(false), _particleTick(0), _weatherChosen(false)
{
	for (int a = 0; a < 3; ++a)
	{
		_eye[a] = _fwd[a] = _right[a] = _up[a] = 0.0;
		_lastHead[a] = 0.0f;
	}
	_depth.resize(width * height);
	_glowMask.resize(width * height);
}

/**
 * Cleans up the first-person view.
 */
FirstPersonView::~FirstPersonView()
{
	delete _spriteBuffer;
	for (Surface *buffer : _blendBuffers) delete buffer;
	{
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		_upscaleStop = true;
	}
	_upscaleWake.notify_all();
	if (_upscaleThread.joinable()) _upscaleThread.join();
	for (auto &entry : _upscaled) delete entry.second.surface;
}

/**
 * Sets the battle to render. Voxel models are rebuilt lazily.
 */
void FirstPersonView::setSavedBattleGame(SavedBattleGame *save)
{
	_save = save;
	_models.clear();
	_frames.clear();
	_lastWalls[0].clear();
	_lastWalls[1].clear();
	_doorAnimations.clear();
	_particles.clear();
	_seenExplosions.clear();
	_lastProjectile = nullptr;
	_weatherChosen = false;
	_ground = nullptr;
	_glides.clear();
	_vents.clear();
	_beams.clear();
	_ufoDoorOpening.clear();
	_flares.clear();
	_watch.clear();
	_decals.clear();
	_scorches.clear();
	_burning.clear();
	for (auto &parts : _lastParts) parts.clear();
	_groundChosen = false;
	_cameraUnit = nullptr;
	preloadModels();
	findMirrors();
}

/**
 * Builds every voxel model the battle can show before it's needed, so nothing is built while
 * playing (turning towards new terrain, a wall blown into its rubble, a door opening): every
 * record of the map's terrain sets in each animation frame, and the sliding doors' cut-outs.
 */
void FirstPersonView::preloadModels()
{
	if (!_save || _cacheOwner)
	{
		return; // a second view (the soldier card) shares the main view's models
	}
	const Uint32 start = SDL_GetTicks();
	for (MapDataSet *set : *_save->getMapDataSets())
	{
		for (size_t i = 0; i < set->getSize(); ++i)
		{
			const MapData *md = set->getObject(i);
			const Frames &frames = getFrames(md);
			for (int f = 0; f < 8; ++f)
			{
				getModel(md, frames.sprite[f], false);
				if (md->isUFODoor() && f > 0) getModel(md, frames.sprite[f], true);
			}
		}
	}
	Log(LOG_INFO) << "OpenXcom3D: " << _models.size() << " voxel models ready in " << SDL_GetTicks() - start << " ms";
}

/**
 * Finds the crafts and UFOs on the map: tiles built from the terrains of crafts and UFOs (and of
 * no other terrain), connected. The iso art shows only their east and south sides; their west
 * and north sides show whatever the projection puts there (the Skyranger's west flank is a thin
 * shell drawn only from the inside, its east one a solid block). Crafts are symmetric, so a
 * hidden side can borrow the colours of the side opposite: for each craft, whether its shape
 * mirrors east-west and north-south, and for each row of voxels the open east / south faces.
 */
void FirstPersonView::findMirrors()
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ(), count = sx * sy * sz;
	_mirrors.clear();
	_mirrorOf.assign(count, -1);
	const Mod *mod = _save->getMod();
	if (!mod)
	{
		return;
	}
	const Uint32 begin = SDL_GetTicks();
	std::set<std::string> structure, landscape;
	auto collect = [](RuleTerrain *terrain, std::set<std::string> &into)
	{
		if (!terrain) return;
		for (const MapDataSet *set : *terrain->getMapDataSets()) into.insert(set->getName());
	};
	for (const auto &type : mod->getCraftsList()) collect(mod->getCraft(type)->getBattlescapeTerrainData(), structure);
	for (const auto &type : mod->getUfosList()) collect(mod->getUfo(type)->getBattlescapeTerrainData(), structure);
	for (const auto &type : mod->getTerrainList()) collect(mod->getTerrain(type), landscape);
	for (const auto &name : landscape) structure.erase(name); // (BLANKS and the like are everywhere)
	if (structure.empty())
	{
		return;
	}
	// each tile's parts as first drawn; a tile is part of a craft if any of them comes from one
	std::vector<std::array<const VoxelModel*, 4>> models(count);
	std::vector<char> craft(count, 0);
	for (int i = 0; i < count; ++i)
	{
		const Tile *tile = _save->getTile(i);
		for (int part = 0; part < 4; ++part)
		{
			const MapData *md = tile->getMapData((TilePart)part);
			const VoxelModel *model = md ? getModel(md, -1, false) : nullptr;
			models[i][part] = model && !model->empty ? model : nullptr;
			if (models[i][part] && md->getDataset() && structure.count(md->getDataset()->getName())) craft[i] = 1;
		}
	}
	// solid at a world voxel: anything (owner < 0) or only the given craft's tiles
	auto solid = [&](int wx, int wy, int wz, int owner)
	{
		if (wx < 0 || wy < 0 || wz < 0 || wx >= sx * 16 || wy >= sy * 16 || wz >= sz * 24) return false;
		const int i = ((wz / 24) * sy + wy / 16) * sx + wx / 16;
		if (owner >= 0 && _mirrorOf[i] != owner) return false;
		for (const VoxelModel *m : models[i])
		{
			if (m && m->color[wz % 24][wy % 16][wx % 16]) return true;
		}
		return false;
	};
	std::vector<int> component, stack;
	for (int seed = 0; seed < count; ++seed)
	{
		if (!craft[seed] || _mirrorOf[seed] >= 0 || _mirrors.size() >= 64) continue;
		const int id = (int)_mirrors.size();
		_mirrors.emplace_back();
		Mirror &mirror = _mirrors.back();
		// its connected tiles and the box of its voxels
		component.clear();
		stack.assign(1, seed);
		_mirrorOf[seed] = id;
		int lo[3] = { INT_MAX, INT_MAX, INT_MAX }, hi[3] = { INT_MIN, INT_MIN, INT_MIN };
		while (!stack.empty())
		{
			const int i = stack.back();
			stack.pop_back();
			component.push_back(i);
			const int t[3] = { i % sx, (i / sx) % sy, i / (sx * sy) };
			for (const VoxelModel *m : models[i])
			{
				if (!m) continue;
				for (int a = 0; a < 3; ++a)
				{
					const int scale = a < 2 ? 16 : 24;
					lo[a] = std::min(lo[a], t[a] * scale + m->min[a]);
					hi[a] = std::max(hi[a], t[a] * scale + m->max[a]);
				}
			}
			for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = t[0] + dx, ny = t[1] + dy, nz = t[2] + dz;
						if (nx < 0 || ny < 0 || nz < 0 || nx >= sx || ny >= sy || nz >= sz) continue;
						const int n = (nz * sy + ny) * sx + nx;
						if (craft[n] && _mirrorOf[n] < 0) { _mirrorOf[n] = id; stack.push_back(n); }
					}
		}
		for (int a = 0; a < 3; ++a)
		{
			mirror.lo[a] = lo[a];
			mirror.size[a] = hi[a] - lo[a] + 1;
		}
		for (int axis = 0; axis < 2; ++axis)
		{
			mirror.sum[axis] = lo[axis] + hi[axis];
			// does it mirror? (most of its voxels have a twin across the middle)
			int voxels = 0, twins = 0;
			for (const int i : component)
			{
				const int t[3] = { i % sx, (i / sx) % sy, i / (sx * sy) };
				for (int vz = 0; vz < 24; ++vz)
					for (int vy = 0; vy < 16; ++vy)
						for (int vx = 0; vx < 16; ++vx)
						{
							int w[3] = { t[0] * 16 + vx, t[1] * 16 + vy, t[2] * 24 + vz };
							if (!solid(w[0], w[1], w[2], id)) continue;
							++voxels;
							w[axis] = mirror.sum[axis] - w[axis];
							twins += solid(w[0], w[1], w[2], id);
						}
			}
			mirror.symmetric[axis] = voxels > 0 && twins * 10 >= voxels * 7;
			// the open east (south) faces along every row of voxels
			const int along = mirror.size[axis], across = mirror.size[1 - axis], rows = across * mirror.size[2];
			mirror.start[axis].assign(rows + 1, 0);
			mirror.pos[axis].clear();
			mirror.outer[axis].assign(rows, -1);
			if (!mirror.symmetric[axis]) continue;
			for (int row = 0; row < rows; ++row)
			{
				mirror.start[axis][row] = (int)mirror.pos[axis].size();
				int w[3];
				w[2] = lo[2] + row / across;
				w[1 - axis] = lo[1 - axis] + row % across;
				for (int k = 0; k < along; ++k)
				{
					w[axis] = lo[axis] + k;
					if (!solid(w[0], w[1], w[2], id)) continue;
					if (mirror.outer[axis][row] < 0) mirror.outer[axis][row] = (Sint16)w[axis];
					int next[3] = { w[0], w[1], w[2] };
					++next[axis];
					if (!solid(next[0], next[1], next[2], -1)) mirror.pos[axis].push_back((Sint16)w[axis]);
				}
			}
			mirror.start[axis][rows] = (int)mirror.pos[axis].size();
		}
		Log(LOG_INFO) << "OpenXcom3D: craft/UFO of " << component.size() << " tiles, mirrors east-west: " << (mirror.symmetric[0] ? "yes" : "no") << ", north-south: " << (mirror.symmetric[1] ? "yes" : "no");
	}
	if (!_mirrors.empty())
	{
		Log(LOG_INFO) << "OpenXcom3D: crafts/UFOs mapped in " << SDL_GetTicks() - begin << " ms";
	}
}

/**
 * The colour of whatever is solid at a world voxel this frame, 0 if nothing.
 */
Uint8 FirstPersonView::colorAt(int wx, int wy, int wz) const
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ();
	if (wx < 0 || wy < 0 || wz < 0 || wx >= sx * 16 || wy >= sy * 16 || wz >= sz * 24) return 0;
	const TileView &tv = _tileViews[((wz / 24) * sy + wy / 16) * sx + wx / 16];
	for (const VoxelModel *m : tv.part)
	{
		const Uint8 c = m ? m->color[wz % 24][wy % 16][wx % 16] : 0;
		if (c) return c;
	}
	return 0;
}

/**
 * For a craft's west-facing (axis 0) or north-facing (axis 1) side: the colour of the surface
 * across the craft's middle on the same row: for its outer skin the outer skin opposite, for
 * anything inside the nearest open east / south face to the mirrored spot (walls differ in
 * thickness). A row with nothing there borrows from the rows next to it; 0 if none have
 * anything or the craft doesn't mirror that way. With skinOnly, only for the outer skin.
 */
Uint8 FirstPersonView::mirroredColor(const Hit &hit, int axis, bool skinOnly) const
{
	const int id = _mirrorOf.empty() ? -1 : _mirrorOf[hit.tile];
	if (id < 0 || !_mirrors[id].symmetric[axis])
	{
		return 0;
	}
	const Mirror &mirror = _mirrors[id];
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY();
	int w[3] = { (hit.tile % sx) * 16 + hit.voxel[0], ((hit.tile / sx) % sy) * 16 + hit.voxel[1], (hit.tile / (sx * sy)) * 24 + hit.voxel[2] };
	const int across = w[1 - axis] - mirror.lo[1 - axis], up = w[2] - mirror.lo[2], width = mirror.size[1 - axis];
	if (across < 0 || across >= width || up < 0 || up >= mirror.size[2])
	{
		return 0;
	}
	const bool skin = mirror.outer[axis][up * width + across] == w[axis];
	if (skinOnly && !skin)
	{
		return 0;
	}
	const int target = mirror.sum[axis] - w[axis];
	static const int Near[][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -2, 0 }, { 2, 0 }, { -3, 0 }, { 3, 0 } };
	for (const auto &step : Near)
	{
		const int a = across + step[0], u = up + step[1];
		if (a < 0 || a >= width || u < 0 || u >= mirror.size[2]) continue;
		const int row = u * width + a;
		const Sint16 *first = mirror.pos[axis].data() + mirror.start[axis][row], *last = mirror.pos[axis].data() + mirror.start[axis][row + 1];
		if (first == last) continue;
		int best = -1;
		if (skin)
		{
			if (std::abs(last[-1] - target) <= 16) best = last[-1];
		}
		else
		{
			const Sint16 *at = std::lower_bound(first, last, (Sint16)target);
			int bestDist = 17; // (no further than a tile from the mirrored spot)
			if (at != last && *at - target < bestDist) { best = *at; bestDist = *at - target; }
			if (at != first && target - at[-1] < bestDist) best = at[-1];
		}
		if (best < 0) continue;
		int v[3] = { w[0], w[1], w[2] };
		v[axis] = best;
		v[1 - axis] = mirror.lo[1 - axis] + a;
		v[2] = mirror.lo[2] + u;
		const Uint8 c = colorAt(v[0], v[1], v[2]); // (0 if it has been blown away since)
		if (c) return c;
	}
	return 0;
}

void FirstPersonView::setEye(double x, double y, double z)
{
	_eye[0] = x;
	_eye[1] = y;
	_eye[2] = z;
}

void FirstPersonView::getEye(double out[3]) const
{
	for (int a = 0; a < 3; ++a)
	{
		out[a] = _eye[a];
	}
}

/**
 * Looks along a unit direction, level.
 * @param unitDirection 0 = north (-y), clockwise in 45 degree steps.
 */
void FirstPersonView::lookAlong(int unitDirection)
{
	_yaw = (unitDirection * 45.0 - 90.0) * Pi / 180.0;
	_pitch = 0.0;
}

/**
 * Turns the camera.
 * @param yawDegrees Positive turns right.
 * @param pitchDegrees Positive looks up.
 */
void FirstPersonView::rotate(double yawDegrees, double pitchDegrees)
{
	_yaw = std::remainder(_yaw + yawDegrees * Pi / 180.0, 2.0 * Pi);
	_pitch = std::clamp(_pitch + pitchDegrees * Pi / 180.0, -80.0 * Pi / 180.0, 80.0 * Pi / 180.0);
}

/**
 * Points the camera at a point in voxels.
 */
void FirstPersonView::lookAt(const double target[3])
{
	const double dx = target[0] - _eye[0], dy = target[1] - _eye[1], dz = target[2] - _eye[2];
	const double flat = std::sqrt(dx * dx + dy * dy);
	if (flat < 1e-6 && std::abs(dz) < 1e-6)
	{
		return;
	}
	_yaw = std::atan2(dy, dx);
	_pitch = std::clamp(std::atan2(dz, flat), -80.0 * Pi / 180.0, 80.0 * Pi / 180.0);
}

double FirstPersonView::getYawDegrees() const
{
	return _yaw * 180.0 / Pi;
}

double FirstPersonView::getPitchDegrees() const
{
	return _pitch * 180.0 / Pi;
}

void FirstPersonView::setFieldOfView(double degrees)
{
	_fovTan = std::tan(std::clamp(degrees, 30.0, 120.0) * Pi / 360.0);
}

void FirstPersonView::setMaxDistance(int tiles)
{
	_maxDistance = (float)(tiles * Position::TileXY);
}

/**
 * Camera basis; x east, y south, z up is a left-handed frame, hence up = fwd x right.
 */
void FirstPersonView::updateBasis()
{
	const double pitch = std::clamp(_pitch + _kick * Pi / 180.0, -80.0 * Pi / 180.0, 80.0 * Pi / 180.0); // recoil shows, it doesn't stay
	const double cy = std::cos(_yaw), sy = std::sin(_yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	_fwd[0] = cy * cp; _fwd[1] = sy * cp; _fwd[2] = sp;
	_right[0] = -sy; _right[1] = cy; _right[2] = 0.0;
	_up[0] = -_fwd[2] * _right[1];
	_up[1] = _fwd[2] * _right[0];
	_up[2] = _fwd[0] * _right[1] - _fwd[1] * _right[0];
	const double fz = _fwd[2] / ZScale, flen = std::sqrt(_fwd[0] * _fwd[0] + _fwd[1] * _fwd[1] + fz * fz);
	_fwdVoxel[0] = _fwd[0] / flen; _fwdVoxel[1] = _fwd[1] / flen; _fwdVoxel[2] = fz / flen;
}

/**
 * Gets the direction of the ray through a pixel of the view.
 */
void FirstPersonView::getRayDirection(int x, int y, double out[3])
{
	updateBasis();
	const int w = getWidth(), h = getHeight();
	const double u = ((x + 0.5) * 2.0 / w - 1.0) * _fovTan;
	const double v = (1.0 - (y + 0.5) * 2.0 / h) * _fovTan * h / w;
	double len = 0.0;
	for (int a = 0; a < 3; ++a)
	{
		out[a] = _fwd[a] + u * _right[a] + v * _up[a];
	}
	out[2] /= ZScale; // into the voxel grid
	for (int a = 0; a < 3; ++a)
	{
		len += out[a] * out[a];
	}
	len = std::sqrt(len);
	for (int a = 0; a < 3; ++a)
	{
		out[a] /= len;
	}
}

/**
 * Gets (building on first use) the voxel colour model for a MapData record shown with a
 * given sprite (animation frame). Cut-out models drop the voxels whose sprite pixel is
 * transparent: used for the frames of a UFO door sliding open.
 */
const FirstPersonView::VoxelModel *FirstPersonView::getModel(const MapData *md, int sprite, bool cutout)
{
	if (_cacheOwner)
	{
		return _cacheOwner->getModel(md, sprite, cutout);
	}
	if (sprite < 0)
	{
		sprite = md->getSprite(0);
	}
	const Uint64 key = ((Uint64)(uintptr_t)md << 16) | ((Uint64)(sprite & 0x7FFF) << 1) | (cutout ? 1 : 0);
	auto it = _models.find(key);
	if (it == _models.end())
	{
		it = _models.emplace(key, VoxelModel()).first;
		buildModel(md, sprite, cutout, it->second);
	}
	return &it->second;
}

/**
 * Gets the sprite frames of a MapData record and whether it animates.
 */
const FirstPersonView::Frames &FirstPersonView::getFrames(const MapData *md)
{
	if (_cacheOwner)
	{
		return _cacheOwner->getFrames(md);
	}
	auto it = _frames.find(md);
	if (it == _frames.end())
	{
		Frames frames;
		frames.animated = false;
		SurfaceSet *set = md->getDataset() ? md->getDataset()->getSurfaceset() : nullptr;
		for (int i = 0; i < 8; ++i)
		{
			frames.sprite[i] = md->getSprite(i);
			const Surface *surface = set ? set->getFrame(frames.sprite[i]) : nullptr;
			frames.buffer[i] = surface ? surface->getBuffer() : nullptr;
			frames.animated |= frames.sprite[i] != frames.sprite[0];
		}
		it = _frames.emplace(md, frames).first;
	}
	return it->second;
}

/**
 * Builds the voxel colour model of a MapData record: solid voxels come from its LOFT
 * templates (exactly like TileEngine::voxelCheck), colours from sampling the isometric
 * sprite at the pixel each voxel projects to.
 */
void FirstPersonView::buildModel(const MapData *md, int spriteIndex, bool cutout, VoxelModel &model) const
{
	std::fill(&model.color[0][0][0], &model.color[0][0][0] + sizeof(model.color), (Uint8)0);
	std::fill(&model.glass[0][0], &model.glass[0][0] + 24 * 16, (Uint16)0);
	model.glow[0] = model.glow[1] = model.glow[2] = 0.0f;
	model.glowCount = 0;
	model.glowColor = 0;
	model.glowMinZ = 0;
	std::fill(&model.plan[0][0], &model.plan[0][0] + 16 * 16, (Uint8)0);
	std::fill(&model.east[0][0], &model.east[0][0] + 24 * 16, (Uint8)0);
	std::fill(&model.south[0][0], &model.south[0][0] + 24 * 16, (Uint8)0);
	model.empty = true;
	for (int a = 0; a < 3; ++a)
	{
		model.min[a] = 255;
		model.max[a] = 0;
	}

	const Surface *sprite = nullptr;
	if (md->getDataset() && md->getDataset()->getSurfaceset())
	{
		sprite = md->getDataset()->getSurfaceset()->getFrame(spriteIndex);
	}
	if (!sprite)
	{
		return;
	}

	// most common colour of the sprite, for voxels that project onto transparent pixels;
	// a sprite with no pixels at all is invisible in the isometric view too, so keep it empty
	int histogram[256] = {};
	for (int y = 0; y < sprite->getHeight(); ++y)
	{
		for (int x = 0; x < sprite->getWidth(); ++x)
		{
			histogram[sprite->getPixel(x, y)]++;
		}
	}
	const Uint8 fallback = (Uint8)(std::max_element(histogram + 1, histogram + 256) - histogram);
	if (histogram[fallback] == 0)
	{
		return;
	}

	// is this a window (declared by the mod), and which of its colours are glass?
	const std::bitset<256> *glass = nullptr;
	if (const RuleFirstPerson::Windows *windows = _game->getMod()->getFirstPerson().getWindows(md->getDataset()->getName()))
	{
		MapDataSet *set = md->getDataset();
		for (size_t i = 0; i < set->getSize(); ++i)
		{
			if (set->getObject(i) == md)
			{
				if (windows->records.count((int)i)) glass = &windows->glass;
				break;
			}
		}
	}

	const std::vector<Uint16> *voxels = _game->getMod()->getVoxelData();
	const int yOffset = md->getYOffset();
	static const int nudges[] = { 0, -1, 1, -2, 2, -3, 3, -4, 4, -6, 6, -8, 8, -12, 12 };
	const int nudgeCount = cutout ? 3 : (int)(sizeof(nudges) / sizeof(nudges[0]));
	for (int z = 0; z < Position::TileZ; ++z)
	{
		const size_t loft = md->getLoftID(z / 2) * 16;
		if (loft + 16 > voxels->size())
		{
			continue;
		}
		for (int y = 0; y < Position::TileXY; ++y)
		{
			const Uint16 row = (*voxels)[loft + y];
			if (!row)
			{
				continue;
			}
			for (int x = 0; x < Position::TileXY; ++x)
			{
				if (!(row & (1 << (15 - x))))
				{
					continue;
				}
				// where this voxel lands in the 32x40 sprite (top corner of the floor diamond is row 24)
				const int px = std::clamp(16 + x - y, 0, sprite->getWidth() - 1);
				const int py = 24 + (x + y) / 2 - z + yOffset;
				Uint8 color = 0;
				for (int i = 0; i < nudgeCount && !color; ++i)
				{
					color = sprite->getPixel(px, py + nudges[i]);
				}
				if (!color)
				{
					if (cutout) continue; // that part of the door has slid away
					color = fallback;
				}
				model.color[z][y][x] = color;
				if (glass && glass->test(color))
				{
					model.glass[z][y] |= (Uint16)(1 << x);
				}
				model.empty = false;
				const int v[3] = { x, y, z };
				for (int a = 0; a < 3; ++a)
				{
					model.min[a] = std::min<int>(model.min[a], v[a]);
					model.max[a] = std::max<int>(model.max[a], v[a]);
				}
			}
		}
	}
	// a thin plate (a floor, a roof) the art draws as a raised slab: its top lands on the slab's
	// side edge in the sprite (the Skyranger's roof plates, a band of cabin trim on every tile).
	// Lift the top's projection until its outline meets the top of what the sprite draws
	if (!model.empty && model.max[2] <= 3 && !cutout)
	{
		int top[32];
		for (int px = 0; px < 32; ++px)
		{
			top[px] = -1;
			for (int py = 0; px < sprite->getWidth() && py < sprite->getHeight(); ++py)
			{
				if (sprite->getPixel(px, py)) { top[px] = py; break; }
			}
		}
		int outline[32];
		auto misfit = [&](int lift)
		{
			std::fill(outline, outline + 32, INT_MAX);
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					const int z = model.max[2];
					if (!model.color[z][y][x]) continue;
					const int px = std::clamp(16 + x - y, 0, 31);
					outline[px] = std::min(outline[px], 24 + (x + y) / 2 - z + yOffset - lift);
				}
			int sum = 0, columns = 0;
			for (int px = 0; px < 32; ++px)
			{
				if (outline[px] == INT_MAX || top[px] < 0) continue;
				sum += std::abs(top[px] - outline[px]);
				++columns;
			}
			return columns ? sum * 16 / columns : INT_MAX;
		};
		int best = 0, bestMisfit = misfit(0);
		for (int lift = 1; lift <= 12; ++lift)
		{
			const int m = misfit(lift);
			if (m < bestMisfit) { best = lift; bestMisfit = m; }
		}
		if (best > 0 && bestMisfit * 2 < misfit(0)) // (misfit(0) leaves the unlifted outline behind)
		{
			// each column of the sprite by its own amount: a curved roof is drawn higher on one side
			int lift[32];
			for (int px = 0; px < 32; ++px)
			{
				lift[px] = outline[px] != INT_MAX && top[px] >= 0 ? std::clamp(outline[px] - top[px], 0, 12) : best;
			}
			const int z = model.max[2];
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					if (!model.color[z][y][x]) continue;
					const int px = std::clamp(16 + x - y, 0, sprite->getWidth() - 1), py = 24 + (x + y) / 2 - z + yOffset - lift[std::min(px, 31)];
					Uint8 color = 0;
					for (int i = 0; i < nudgeCount && !color; ++i) color = sprite->getPixel(px, py + nudges[i]);
					if (color) model.color[z][y][x] = color;
				}
		}
	}
	// the underside (a ceiling seen from below, the bottom of a table) shows the pattern seen from
	// above, column by column: its own voxels would show the top's shading smeared down the
	// projection in diagonal bands
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			for (int z = 23; z >= 0; --z)
			{
				if (model.color[z][y][x]) { model.plan[y][x] = model.color[z][y][x]; break; }
			}
	// a plate's underside is a ceiling the art never shows (and its top often runs onto a trim
	// drawn along its edge): one plain colour, the one of its top's colours nearest their average
	model.ceiling = 0;
	const SDL_Palette *palette = const_cast<Surface*>(sprite)->getSurface()->format->palette;
	if (!model.empty && model.max[2] <= 3 && palette && palette->ncolors >= 256)
	{
		int sum[3] = {}, n = 0;
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 c = model.plan[y][x];
				if (!c) continue;
				sum[0] += palette->colors[c].r; sum[1] += palette->colors[c].g; sum[2] += palette->colors[c].b;
				++n;
			}
		int bestDist = INT_MAX;
		for (int y = 0; y < 16 && n; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 c = model.plan[y][x];
				if (!c) continue;
				const int dr = palette->colors[c].r * n - sum[0], dg = palette->colors[c].g * n - sum[1], db = palette->colors[c].b * n - sum[2];
				const int dist = (std::abs(dr) + std::abs(dg) + std::abs(db)) / n;
				if (dist < bestDist) { bestDist = dist; model.ceiling = c; }
			}
	}
	// likewise the sides the iso art never shows (facing west or north) show what the same row
	// looks like from the side it does show (east or south): a wall has its texture both ways
	for (int z = 0; z < 24; ++z)
		for (int i = 0; i < 16; ++i)
		{
			for (int x = 15; x >= 0; --x)
			{
				if (model.color[z][i][x]) { model.east[z][i] = model.color[z][i][x]; break; }
			}
			for (int y = 15; y >= 0; --y)
			{
				if (model.color[z][y][i]) { model.south[z][i] = model.color[z][y][i]; break; }
			}
		}
	// lit parts: the brightest shades in the top third of the object (a lamp's bulb sits on top)
	if (!model.empty)
	{
		model.glowMinZ = (Uint8)(model.max[2] - (model.max[2] - model.min[2]) / 3);
		for (int z = model.glowMinZ; z <= model.max[2]; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					const Uint8 color = model.color[z][y][x];
					if (!color || (color & 15) > 3) continue;
					model.glow[0] += x + 0.5f;
					model.glow[1] += y + 0.5f;
					model.glow[2] += z + 0.5f;
					if (!model.glowCount++) model.glowColor = color;
				}
		if (model.glowCount)
		{
			for (float &g : model.glow) g /= model.glowCount;
		}
	}
}

/**
 * Notices doors that just opened on a tile (the closed door is replaced by its open
 * version somewhere on the tile) and starts their swing.
 */
void FirstPersonView::trackDoors(int index, Tile *tile)
{
	for (int w = 0; w < 2; ++w)
	{
		const TilePart part = w ? O_NORTHWALL : O_WESTWALL;
		const MapData *now = tile->getMapData(part);
		const MapData *before = _lastWalls[w][index];
		_lastWalls[w][index] = now;
		if (!before || before == now || !before->isDoor() || before->isUFODoor() || !before->getDataset())
		{
			continue;
		}
		const MapData *open = before->getDataset()->getObject(before->getAltMCD());
		for (int p = 0; p < 4; ++p)
		{
			if (open && tile->getMapData((TilePart)p) == open)
			{
				// swing away from the camera when it's on the door's own side, like a door pushed open
				const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY();
				const bool away = part == O_NORTHWALL ? _eye[1] > (index / sx % sy) * 16.0 : _eye[0] > (index % sx) * 16.0;
				_doorAnimations.push_back({ index, part, p, getModel(before), SDL_GetTicks(), away });
				break;
			}
		}
	}
}

/**
 * Is the door in this wall still moving? A normal door swings for a moment after it opened;
 * a UFO door slides through its animation frames until the last one.
 */
bool FirstPersonView::isDoorMoving(const Position &pos, int part)
{
	Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return false;
	}
	const int index = _save->getTileIndex(pos);
	for (const auto &animation : _doorAnimations)
	{
		if (animation.tile == index && animation.part == part && SDL_GetTicks() - animation.start < DoorSwingTime)
		{
			return true;
		}
	}
	const MapData *md = tile->getMapData((TilePart)part);
	if (md && md->isUFODoor() && tile->isUfoDoorOpen((TilePart)part))
	{
		auto opening = _ufoDoorOpening.find(index * 4 + part);
		return opening == _ufoDoorOpening.end() || SDL_GetTicks() - opening->second < UfoDoorTime;
	}
	return false;
}

/**
 * Picks the ground that continues past the map border: the most common floor along the
 * map's outer ring at ground level (grass on a farm, sand in the desert, ...).
 */
void FirstPersonView::chooseGround()
{
	_groundChosen = true;
	std::map<const MapData*, int> counts;
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY();
	for (int y = 0; y < sy; ++y)
	{
		for (int x = 0; x < sx; ++x)
		{
			if (x != 0 && y != 0 && x != sx - 1 && y != sy - 1)
			{
				continue;
			}
			const MapData *floor = _save->getTile(Position(x, y, 0))->getMapData(O_FLOOR);
			if (floor)
			{
				counts[floor]++;
			}
		}
	}
	int best = 0;
	for (const auto &pair : counts)
	{
		const VoxelModel *model = getModel(pair.first);
		if (pair.second > best && !model->empty && model->min[2] == 0)
		{
			best = pair.second;
			_ground = model;
		}
	}
}

/**
 * Takes this frame's snapshot of the map: which voxel model (and animation frame) sits in
 * each tile part, light, fog of war, and doors swinging open. Doors, destruction etc. show
 * up automatically.
 */
void FirstPersonView::snapshotTiles()
{
	const int count = _save->getMapSizeXYZ();
	const bool debug = _save->getDebugMode();
	const bool trackWalls = (int)_lastWalls[0].size() == count;
	if (!trackWalls)
	{
		_lastWalls[0].assign(count, nullptr);
		_lastWalls[1].assign(count, nullptr);
		for (auto &parts : _lastParts) parts.assign(count, nullptr);
		_burning.assign(count, 0);
		_doorAnimations.clear();
	}
	_tileViews.resize(count);
	// the game animates tiles one frame per tick: time since the last tick, over the tick's length
	const Uint32 tick = SDL_GetTicks();
	if (_save->getAnimFrame() != _lastAnimFrame)
	{
		if (_lastAnimFrame >= 0) _animPeriod = std::clamp<Uint32>(tick - _animChanged, 40, 400);
		_lastAnimFrame = _save->getAnimFrame();
		_animChanged = tick;
	}
	_animBlend = std::min(1.0f, (tick - _animChanged) / (float)_animPeriod);
	_lights.clear();
	_devices.clear();
	for (int i = 0; i < count; ++i)
	{
		Tile *tile = _save->getTile(i);
		if (trackWalls)
		{
			trackDoors(i, tile);
			// a fire burnt out here: the ground is left blackened
			const bool burning = tile->getFire() > 0;
			if (_burning[i] && !burning && _scorches.size() < 200)
			{
				const Position at = tile->getPosition();
				_scorches.push_back({ { at.x * 16.0f + 8.0f, at.y * 16.0f + 8.0f, at.z * 24.0f + GroundTop - tile->getTerrainLevel() }, 10.0f });
			}
			_burning[i] = burning;
			// something destroyed here since last frame (not a door opening): debris
			for (int part = 0; part < 4; ++part)
			{
				const MapData *before = _lastParts[part][i], *now = tile->getMapData((TilePart)part);
				_lastParts[part][i] = now;
				if (!before || before == now || before->isDoor() || before->isUFODoor() || !before->getDataset()) continue;
				const MapData *destroyed = before->getDieMCD() ? before->getDataset()->getObject(before->getDieMCD()) : nullptr;
				if (now && now != destroyed) continue;
				const Position at = tile->getPosition();
				const double dx = at.x * 16.0 + 8.0 - _eye[0], dy = at.y * 16.0 + 8.0 - _eye[1];
				if (dx * dx + dy * dy < 480.0 * 480.0) debris(at, part, before);
			}
		}
		else
		{
			_lastWalls[0][i] = tile->getMapData(O_WESTWALL);
			_lastWalls[1][i] = tile->getMapData(O_NORTHWALL);
			for (int part = 0; part < 4; ++part) _lastParts[part][i] = tile->getMapData((TilePart)part);
			_burning[i] = tile->getFire() > 0;
		}
		TileView &tv = _tileViews[i];
		tv.empty = true;
		tv.swing[0] = tv.swing[1] = -1;
		tv.emissive = 0;
		for (int a = 0; a < 3; ++a)
		{
			tv.min[a] = 255;
			tv.max[a] = 0;
		}
		for (int part = 0; part < 4; ++part)
		{
			tv.part[part] = nullptr;
			tv.next[part] = nullptr;
			const MapData *md = tile->getMapData((TilePart)part);
			if (!md || (part == O_FLOOR && (int)i == _hiddenFloor))
			{
				continue;
			}
			// which animation frame is the tile showing? (water, lights, computers, UFO doors...)
			const Frames &frames = getFrames(md);
			int frame = 0;
			if (frames.animated)
			{
				const Uint8 *shown = tile->getSprite((TilePart)part).getBuffer();
				for (int f = 0; f < 8; ++f)
				{
					if (frames.buffer[f] == shown)
					{
						frame = f;
						break;
					}
				}
			}
			// a UFO door opening shows its sliding frames, cut out where the door has gone, at the
			// view's own pace (at least half a second) so it's seen sliding open
			const bool ufoWall = md->isUFODoor() && (part == O_WESTWALL || part == O_NORTHWALL);
			const bool cutout = ufoWall && tile->isUfoDoorOpen((TilePart)part);
			if (cutout)
			{
				auto opening = _ufoDoorOpening.find(i * 4 + part);
				if (opening == _ufoDoorOpening.end())
				{
					// already open when the view first saw it: no replay
					opening = _ufoDoorOpening.emplace(i * 4 + part, trackWalls ? SDL_GetTicks() : SDL_GetTicks() - 10 * UfoDoorTime).first;
				}
				const Uint32 age = SDL_GetTicks() - opening->second;
				frame = age < UfoDoorTime ? std::clamp(1 + (int)(age * 6 / UfoDoorTime), 1, 7) : 7;
			}
			else if (ufoWall)
			{
				_ufoDoorOpening.erase(i * 4 + part);
			}
			const VoxelModel *model = getModel(md, frames.sprite[frame], cutout);
			if (model->empty)
			{
				continue;
			}
			tv.part[part] = model;
			tv.empty = false;
			// the frame it steps to next, to dissolve into over the tick (not for sliding doors)
			if (frames.animated && !ufoWall && frames.sprite[(frame + 1) % 8] != frames.sprite[frame])
			{
				const VoxelModel *next = getModel(md, frames.sprite[(frame + 1) % 8], false);
				if (!next->empty) tv.next[part] = next;
			}
			if (md->getLightSource() > 0 && model->glowCount)
			{
				tv.emissive |= (Uint8)(1 << part);
			}
			for (int a = 0; a < 3; ++a)
			{
				tv.min[a] = std::min(tv.min[a], model->min[a]);
				tv.max[a] = std::max(tv.max[a], model->max[a]);
			}
		}
		tv.light = (Uint8)std::clamp(tile->getShade(), 0, _nightVision ? 4 : 15);
		tv.discovered = debug || tile->isDiscovered(O_FLOOR);
		tv.seen = (Uint8)((tv.discovered ? (1 << O_FLOOR) | (1 << O_OBJECT) : 0)
			| (debug || tile->isDiscovered(O_WESTWALL) ? 1 << O_WESTWALL : 0)
			| (debug || tile->isDiscovered(O_NORTHWALL) ? 1 << O_NORTHWALL : 0));
		// light sources and UFO machinery near the camera, for light pools and particles (a lamp
		// shines on what you see even if its own tile is still unexplored)
		if (!tv.empty)
		{
			const Position pos = tile->getPosition();
			const float ox = pos.x * 16.0f, oy = pos.y * 16.0f, oz = pos.z * 24.0f;
			const float dx = ox + 8.0f - (float)_eye[0], dy = oy + 8.0f - (float)_eye[1];
			if (dx * dx + dy * dy < 480.0f * 480.0f)
			{
				for (int part = 0; part < 4; ++part)
				{
					const VoxelModel *m = tv.part[part];
					const MapData *md = tile->getMapData((TilePart)part);
					if (!m || !md) continue;
					if ((tv.emissive >> part) & 1)
					{
						LightSpot light = { { ox + m->glow[0], oy + m->glow[1], oz + m->glow[2] }, std::min(1.0f, md->getLightSource() / 12.0f), m->glowColor };
						_lights.push_back(light);
					}
					const SpecialTileType special = md->getSpecialType();
					if (tv.discovered && (special == UFO_POWER_SOURCE || special == UFO_NAVIGATION) && dx * dx + dy * dy < 240.0f * 240.0f)
					{
						DeviceSpot device;
						for (int a = 0; a < 3; ++a)
						{
							const float origin = a == 0 ? ox : a == 1 ? oy : oz;
							device.pos[a] = origin + (m->min[a] + m->max[a] + 1) * 0.5f;
							device.half[a] = (m->max[a] + 1 - m->min[a]) * 0.5f;
						}
						device.power = special == UFO_POWER_SOURCE;
						_devices.push_back(device);
					}
				}
			}
		}
	}

	// every shaded pixel looks at the lamps: keep the ones nearest the camera
	const size_t maxLights = 16;
	if (_lights.size() > maxLights)
	{
		const float ex = (float)_eye[0], ey = (float)_eye[1];
		std::partial_sort(_lights.begin(), _lights.begin() + maxLights, _lights.end(), [ex, ey](const LightSpot &a, const LightSpot &b) {
			return (a.pos[0] - ex) * (a.pos[0] - ex) + (a.pos[1] - ey) * (a.pos[1] - ey) < (b.pos[0] - ex) * (b.pos[0] - ex) + (b.pos[1] - ey) * (b.pos[1] - ey);
		});
		_lights.resize(maxLights);
	}

	// doors swinging open: the open door appears only when the swing ends; one swung away
	// from the camera stays there until the camera has gone through (the open door sprite is
	// on the camera's side, so it would pop up in front of it)
	_swings.clear();
	const Uint32 now = SDL_GetTicks();
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY();
	for (auto it = _doorAnimations.begin(); it != _doorAnimations.end();)
	{
		const float p = std::min(1.0f, (now - it->start) / (float)DoorSwingTime);
		const int tx = it->tile % sx, ty = (it->tile / sx) % sy, tz = it->tile / (sx * sy);
		const bool westWall = it->part == O_WESTWALL;
		const bool cameraOnOwnSide = westWall ? _eye[0] > tx * 16.0 : _eye[1] > ty * 16.0;
		const bool hold = it->away && cameraOnOwnSide && now - it->start < 5000;
		if ((p >= 1.0f && !hold) || it->tile >= count || _swings.size() >= 32000)
		{
			it = _doorAnimations.erase(it);
			continue;
		}
		const float eased = p * p * (3.0f - 2.0f * p);
		// west wall: from along +y to +x (or -x when away); north wall: from along +x to +y (or -y)
		const float angle = westWall ? (it->away ? 1.0f + eased : 1.0f - eased) * 1.5707963f : (it->away ? -eased : eased) * 1.5707963f;
		DoorSwing swing;
		swing.hinge[0] = tx * 16.0f + 1.0f;
		swing.hinge[1] = ty * 16.0f + 1.0f;
		swing.dir[0] = std::cos(angle);
		swing.dir[1] = std::sin(angle);
		swing.z0 = tz * 24.0f + it->model->min[2];
		swing.z1 = tz * 24.0f + it->model->max[2] + 1;
		swing.model = it->model;
		swing.westWall = westWall;
		TileView &tv = _tileViews[it->tile];
		tv.part[it->hiddenPart] = nullptr;
		tv.next[it->hiddenPart] = nullptr;
		tv.swing[0] = (Sint16)_swings.size();
		tv.empty = false;
		// the swing reaches into the neighbour on the far side of the wall
		const int nx = westWall ? tx - 1 : tx, ny = westWall ? ty : ty - 1;
		if (it->away && nx >= 0 && ny >= 0)
		{
			TileView &neighbour = _tileViews[(tz * sy + ny) * sx + nx];
			neighbour.swing[neighbour.swing[0] < 0 ? 0 : 1] = (Sint16)_swings.size();
			neighbour.empty = false;
		}
		_swings.push_back(swing);
		++it;
	}
}

/**
 * Picks this frame's sky, haze and border colours from the current palette (so UFO and
 * TFTD palettes both work), and the sun/moon and face shading for the time of day.
 */
void FirstPersonView::updateLook()
{
	const SDL_Color *pal = getPalette();
	auto nearest = [pal](float r, float g, float b) {
		int best = 1;
		float bestDist = 1e9f;
		for (int i = 1; i < 256; ++i)
		{
			const float dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
			const float dist = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
			if (dist < bestDist)
			{
				bestDist = dist;
				best = i;
			}
		}
		return (Uint8)best;
	};
	auto mix = [](const float *a, const float *b, float t, float *out) {
		for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
	};

	const int shade = std::clamp(_save->getGlobalShade(), 0, 15);
	_look.night = std::clamp((shade - 3) / 9.0f, 0.0f, 1.0f);
	_look.underwater = _save->getDepth() > 0;
	_look.ambient = _nightVision ? std::min(shade, 4) : shade;
	_look.cloudTime = SDL_GetTicks() / 1000.0f;

	static const float dayHorizon[3] = { 190, 210, 230 }, dayZenith[3] = { 70, 120, 200 };
	static const float nightHorizon[3] = { 24, 28, 50 }, nightZenith[3] = { 3, 4, 14 };
	static const float waterHorizon[3] = { 8, 34, 46 }, waterZenith[3] = { 34, 104, 118 };
	static const float cloudLit[3] = { 240, 240, 245 }, cloudDark[3] = { 140, 146, 162 };
	static const float cloudNightLit[3] = { 46, 48, 66 }, cloudNightDark[3] = { 20, 22, 34 };
	float horizon[3], zenith[3], c0[3], c1[3];
	if (_look.underwater)
	{
		mix(waterHorizon, waterHorizon, 0, horizon);
		mix(waterZenith, waterZenith, 0, zenith);
	}
	else
	{
		mix(dayHorizon, nightHorizon, _look.night, horizon);
		mix(dayZenith, nightZenith, _look.night, zenith);
	}
	for (int i = 0; i < 32; ++i)
	{
		float c[3];
		mix(horizon, zenith, i / 31.0f, c);
		_look.sky[i] = nearest(c[0], c[1], c[2]);
	}
	mix(cloudLit, cloudNightLit, _look.night, c0);
	mix(cloudDark, cloudNightDark, _look.night, c1);
	for (int i = 0; i < 8; ++i)
	{
		float c[3];
		mix(c0, c1, i / 7.0f, c);
		_look.cloud[i] = nearest(c[0], c[1], c[2]);
	}
	_look.haze = _look.sky[0];
	_look.star = nearest(230, 230, 240);
	_look.moon = nearest(228, 228, 210);
	_look.sun = nearest(255, 248, 200);
	_look.fire[0] = nearest(255, 250, 200);
	_look.fire[1] = nearest(255, 210, 60);
	_look.fire[2] = nearest(240, 120, 20);
	_look.fire[3] = nearest(160, 40, 10);
	_look.smoke = nearest(130, 130, 130);
	_look.spark = nearest(255, 240, 160);
	_look.blood[0] = nearest(90, 0, 0);
	_look.blood[1] = nearest(160, 12, 8);
	_look.dust = nearest(200, 190, 170);
	_look.rain = nearest(150, 170, 200);
	_look.snow = nearest(240, 240, 250);
	_look.sand = nearest(210, 170, 110);
	_look.firefly = nearest(200, 255, 100);
	_look.bubble = nearest(170, 230, 240);
	_look.tapeYellow = nearest(240, 200, 30);
	_look.marker = nearest(235, 40, 30);
	_look.marine = nearest(150, 195, 190);
	_look.flareFire[0] = nearest(240, 30, 20);
	_look.flareFire[1] = nearest(255, 100, 30);
	_look.flareFire[2] = nearest(255, 200, 80);
	_look.tapeBlack = nearest(20, 20, 20);
	_look.post = nearest(110, 110, 110);

	// how each colour looks through window glass: a little paler and bluer
	Uint32 hash = 2166136261u;
	for (int i = 0; i < 256; ++i)
	{
		hash = (hash ^ (pal[i].r | (pal[i].g << 8) | (pal[i].b << 16))) * 16777619u;
	}
	if (hash != _look.paletteHash)
	{
		_look.paletteHash = hash;
		_look.glassTint[0] = 0;
		for (int i = 1; i < 256; ++i)
		{
			_look.glassTint[i] = nearest(pal[i].r * 0.7f + 150 * 0.3f, pal[i].g * 0.7f + 180 * 0.3f, pal[i].b * 0.7f + 205 * 0.3f);
			const float lum = pal[i].r * 0.3f + pal[i].g * 0.59f + pal[i].b * 0.11f;
			_look.redTint[i] = nearest(60 + lum * 0.85f, lum * 0.18f, lum * 0.15f);
			_look.blueTint[i] = nearest(lum * 0.1f, lum * 0.16f, 25 + lum * 0.45f);
			_look.orangeTint[i] = nearest(std::min(255.0f, 80 + lum * 0.9f), 30 + lum * 0.45f, lum * 0.08f);
			_look.whiteTint[i] = nearest(std::min(255.0f, 180 + lum * 0.3f), std::min(255.0f, 180 + lum * 0.3f), std::min(255.0f, 180 + lum * 0.3f));
			_look.greenTint[i] = nearest(15 + lum * 0.3f, 35 + lum * 0.7f, lum * 0.2f);
			_look.flareTint[i] = nearest(std::min(255.0f, 70 + lum * 1.15f), lum * 0.32f, lum * 0.24f);
		}
		_look.redTint[0] = 0;
		_look.blueTint[0] = nearest(0, 2, 18);
		_look.orangeTint[0] = nearest(60, 24, 0);
		_look.whiteTint[0] = nearest(185, 185, 185);
		_look.greenTint[0] = nearest(8, 22, 4);
		_look.flareTint[0] = 0;
		for (int k = 0; k < 4; ++k) _look.grey[k] = nearest(30 + 60 * k, 30 + 60 * k, 30 + 60 * k);
	}

	// sun high in the north-west by day; at night only the moon, and faces get a flatter light
	const float sun[3] = { -0.45f, -0.5f, 0.74f }, moon[3] = { 0.35f, -0.75f, 0.56f };
	for (int i = 0; i < 3; ++i)
	{
		_look.sunDir[i] = sun[i];
		_look.moonDir[i] = moon[i];
	}
	const float day = 1.0f - _look.night;
	static const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int n = 0; n < 6; ++n)
	{
		const float lambert = std::max(0.0f, normals[n][0] * sun[0] + normals[n][1] * sun[1] + normals[n][2] * sun[2]);
		_look.faceShade[n] = (1.0f - lambert) * (0.5f + 2.5f * day) + (n == 5 ? 1.0f : 0.0f);
	}
}

/**
 * The game's light (as a shade, 0 brightest .. 15 darkest) at a point, blended between
 * tile centres. Undiscovered tiles don't leak their light; open ground outside the map
 * gets the ambient light.
 */
float FirstPersonView::lightAt(float x, float y, float z) const
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ();
	const int cx = std::clamp((int)std::floor(x / 16), 0, sx - 1);
	const int cy = std::clamp((int)std::floor(y / 16), 0, sy - 1);
	const int cz = std::clamp((int)std::floor(z / 24), 0, sz - 1);
	const float centre = _tileViews[(cz * sy + cy) * sx + cx].light;
	if (!_smoothLighting)
	{
		return centre;
	}
	const float fx = x / 16 - 0.5f, fy = y / 16 - 0.5f, fz = z / 24 - 0.5f;
	const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy), z0 = (int)std::floor(fz);
	const float wx = fx - x0, wy = fy - y0, wz = fz - z0;
	float sum = 0.0f;
	for (int k = 0; k < 8; ++k)
	{
		const int ix = x0 + (k & 1), iy = y0 + ((k >> 1) & 1), iz = std::clamp(z0 + ((k >> 2) & 1), 0, sz - 1);
		float value;
		if (ix < 0 || iy < 0 || ix >= sx || iy >= sy)
		{
			value = (float)_look.ambient;
		}
		else
		{
			const TileView &tv = _tileViews[(iz * sy + iy) * sx + ix];
			value = tv.discovered ? tv.light : centre;
		}
		const float w = ((k & 1) ? wx : 1 - wx) * (((k >> 1) & 1) ? wy : 1 - wy) * (((k >> 2) & 1) ? wz : 1 - wz);
		sum += value * w;
	}
	return sum;
}

/**
 * Lights a terrain hit: the game's light sampled just off the surface, the sun on the face,
 * distance; dithered to whole palette shades, then haze towards the horizon.
 */
/**
 * Is there anything solid on the straight line between two points? (Shadow rays.)
 */
bool FirstPersonView::shadowed(const float from[3], const float to[3]) const
{
	float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (length < 1.0f)
	{
		return false;
	}
	for (float &a : d) a /= length;
	RayContext ctx;
	ctx.tiles = _tileViews.data();
	ctx.swings = _swings.data();
	ctx.size[0] = _save->getMapSizeX();
	ctx.size[1] = _save->getMapSizeY();
	ctx.size[2] = _save->getMapSizeZ();
	ctx.maxDistance = length;
	Hit hit;
	float tEnd;
	int endAxis;
	return castRay(ctx, from, d, hit, tEnd, endAxis) == RAY_HIT && hit.t < length;
}

/**
 * Extra light at a point from the lamps nearby and the shoulder flashlight, in shades
 * (negative: darker, a lamp's shadow is darker than the game's flat light around it).
 * @param shadows Trace shadow rays (terrain); units are lit without them.
 */
float FirstPersonView::pointLight(const float p[3], const float n[3], bool shadows) const
{
	float light = 0.0f;
	// lamps: a warm pool beneath them; what stands in the way throws a shadow
	if (!_lights.empty() && (_look.night > 0.3f || _look.underwater))
	{
		const float radius = 120.0f;
		const LightSpot *best = nullptr;
		float bestPower = 0.0f, bestDist = 0.0f;
		for (const auto &lamp : _lights)
		{
			const float dx = lamp.pos[0] - p[0], dy = lamp.pos[1] - p[1], dz = lamp.pos[2] - p[2];
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist > radius) continue;
			const float power = lamp.strength * (1.0f - dist / radius) * (1.0f - dist / radius);
			if (power > bestPower) { bestPower = power; best = &lamp; bestDist = dist; }
		}
		if (best && bestPower > 0.03f)
		{
			const float dir[3] = { (best->pos[0] - p[0]) / bestDist, (best->pos[1] - p[1]) / bestDist, (best->pos[2] - p[2]) / bestDist };
			const float facing = n ? n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] : 1.0f;
			const float to[3] = { best->pos[0] - dir[0] * 4.0f, best->pos[1] - dir[1] * 4.0f, best->pos[2] - dir[2] * 4.0f };
			const float night = _look.underwater ? 1.0f : _look.night;
			if (facing <= 0.0f || (shadows && shadowed(p, to)))
			{
				light -= bestPower * 3.0f * night;
			}
			else
			{
				light += bestPower * 6.0f * (0.4f + 0.6f * facing) * night;
			}
		}
	}
	// the flashlight: a cone along the view, from the soldier's shoulder
	if (_flashlight)
	{
		const float range = 16.0f * 14.0f;
		const float l[3] = { p[0] - _flash[0], p[1] - _flash[1], p[2] - _flash[2] };
		const float dist = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
		if (dist > 0.5f && dist < range)
		{
			const float cosine = (l[0] * (float)_fwdVoxel[0] + l[1] * (float)_fwdVoxel[1] + l[2] * (float)_fwdVoxel[2]) / dist;
			if (cosine > 0.8f)
			{
				const float cone = smoothstep(0.8f, 0.96f, cosine);
				const float fall = 1.0f - dist / range;
				const float facing = n ? -(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]) / dist : 1.0f;
				const float power = cone * fall * fall * 10.0f * (0.35f + 0.65f * std::max(0.0f, facing));
				if (power > 0.25f && facing > 0.0f && !(shadows && shadowed(p, _flash)))
				{
					light += power;
				}
			}
		}
	}
	return light;
}

Uint8 FirstPersonView::shadeHit(const Hit &hit, const float o[3], const float d[3], int px, int py) const
{
	const TileView &tv = _tileViews[hit.tile];
	// a wall belongs to the tile east or south of it, but it's seen from both sides
	if (hit.part >= 0 ? !((tv.seen >> hit.part) & 1) : !tv.discovered)
	{
		// the underside of an unexplored level seen from an explored room below is just its
		// ceiling: draw it, lit by the room; anything else unexplored is a black silhouette
		const int layer = _save->getMapSizeX() * _save->getMapSizeY();
		const bool ceiling = hit.axis == 2 && d[2] > 0.0f && hit.tile >= layer && _tileViews[hit.tile - layer].discovered;
		if (!ceiling)
		{
			return 0x0F;
		}
	}
	int normal = 4; // +z
	float n[3] = { 0, 0, 1 };
	if (hit.axis >= 0)
	{
		n[2] = 0;
		n[hit.axis] = d[hit.axis] > 0 ? -1.0f : 1.0f;
		normal = hit.axis * 2 + (n[hit.axis] < 0 ? 1 : 0);
	}
	float p[3];
	for (int a = 0; a < 3; ++a)
	{
		p[a] = o[a] + d[a] * hit.t + n[a] * 1.5f;
	}
	float shade = lightAt(p[0], p[1], p[2]) + _look.faceShade[normal] + hit.t / 320.0f;
	float flare = 0.0f; // red light from tactical flares
	if (hit.emissive)
	{
		shade = std::min(shade, 0.5f); // lamps, lit screens and UFO lights shine whatever the light around them
	}
	else if (Options::fpsShadows)
	{
		// the sun: a point facing it with something in the way is in shadow
		const float day = _look.underwater ? 0.0f : 1.0f - _look.night;
		const float facing = n[0] * _look.sunDir[0] + n[1] * _look.sunDir[1] + n[2] * _look.sunDir[2];
		if (day > 0.3f && facing > 0.05f && hit.t < 640.0f)
		{
			const float to[3] = { p[0] + _look.sunDir[0] * 260.0f, p[1] + _look.sunDir[1] * 260.0f, p[2] + _look.sunDir[2] * 260.0f };
			if (shadowed(p, to)) shade += 3.0f * day;
		}
		shade -= pointLight(p, n, true);
	}
	else
	{
		shade -= pointLight(p, n, false);
	}
	if (!hit.emissive && !_flares.empty())
	{
		flare = flareLight(p, n, Options::fpsShadows);
		shade -= flare * 10.0f;
	}
	if (_muzzleLight > 0.0f)
	{
		// a muzzle flash: brief, bright, close (no shadows, it's over in a blink)
		const float dx = p[0] - _muzzle[0], dy = p[1] - _muzzle[1], dz = p[2] - _muzzle[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 < 96.0f * 96.0f)
		{
			const float f = 1.0f - std::sqrt(d2) / 96.0f;
			shade -= f * f * _muzzleLight * (4.0f + 6.0f * _look.night);
		}
	}
	if (!hit.emissive && _look.underwater && normal == 4 && hit.t < 480.0f)
	{
		// sunlight rippling over the sea floor
		const float t = _look.cloudTime;
		const float c = std::sin(p[0] * 0.31f + t * 1.7f) + std::sin(p[1] * 0.27f - t * 1.3f) + std::sin((p[0] - p[1]) * 0.19f + t * 0.9f);
		if (c > 1.3f) shade -= (c - 1.3f) * 1.8f;
	}
	// an animated part part way into its next frame: that frame's colour, dithered in
	Uint8 base = hit.color;
	if (hit.part >= 0 && tv.part[hit.part])
	{
		// a face the iso art never shows: what its row shows from the side it does
		const VoxelModel &m = *tv.part[hit.part];
		const int vx = hit.voxel[0], vy = hit.voxel[1], vz = hit.voxel[2];
		Uint8 seen = hit.axis == 2 && d[2] > 0.0f ? (m.ceiling ? m.ceiling : m.plan[vy][vx]) : 0; // an underside
		const int layer = _save->getMapSizeX() * _save->getMapSizeY();
		if (seen && m.color[23][vy][vx] && hit.tile + layer < (int)_tileViews.size())
		{
			// its top is covered by what's on the level above (a craft's cabin under its roof
			// plates: the art shows the cabin there): the top of that is what this is the underside of
			for (const VoxelModel *above : _tileViews[hit.tile + layer].part)
			{
				if (above && above->color[0][vy][vx] && above->plan[vy][vx]) { seen = above->ceiling ? above->ceiling : above->plan[vy][vx]; break; }
			}
		}
		if (hit.axis >= 0 && hit.axis < 2 && d[hit.axis] > 0.0f)
		{
			// a west- or north-facing side: on a craft the mirrored side, else its own row from the other side
			seen = mirroredColor(hit, hit.axis);
			if (!seen) seen = hit.axis == 0 ? m.east[vz][vy] : m.south[vz][vx];
		}
		else if (hit.axis >= 0 && hit.axis < 2)
		{
			// a south or east face on a craft's hidden west or north skin (where the hull steps
			// in or out): the art sees it through the hull, so it too takes the mirrored side
			seen = mirroredColor(hit, 1 - hit.axis, true);
		}
		if (seen) base = seen;
	}
	if (hit.part >= 0 && tv.next[hit.part] && dither(px + 2, py + 1) < _animBlend)
	{
		const Uint8 next = tv.next[hit.part]->color[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		if (next) base = next;
	}
	Uint8 color = shadeColor(base, (int)std::max(0.0f, shade + dither(px, py)));
	if (flare * 1.5f > 0.04f + dither(px + 1, py + 3))
	{
		color = _look.flareTint[color]; // the flare's light is red
	}
	// under the sea the murk closes in much sooner
	const float haze = _look.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, hit.t)
		: smoothstep(_maxDistance * 0.45f, _maxDistance, hit.t);
	if (haze > dither(px + 2, py + 1))
	{
		color = _look.haze;
	}
	return color;
}

/**
 * What a ray sees after leaving the map through its side: the X-COM tape on its posts
 * along the border, then the map's ground running on to the horizon, then sky.
 */
Uint8 FirstPersonView::outsideColor(const float o[3], const float d[3], float tExit, int exitAxis, int px, int py, float &depth) const
{
	const float p[3] = { o[0] + d[0] * tExit, o[1] + d[1] * tExit, o[2] + d[2] * tExit };
	const float along = exitAxis == 0 ? p[1] : p[0];
	const float tapeBottom = GroundTop + 8.0f;
	const float shade = _look.ambient * 0.5f;
	const float postPhase = along - std::floor(along / 64.0f) * 64.0f;
	if (postPhase < 1.5f && p[2] >= GroundTop && p[2] < GroundTop + 17.0f)
	{
		depth = tExit;
		return shadeColor(_look.post, (int)(shade + (exitAxis == 0 ? 1.0f : 2.0f) + dither(px, py)));
	}
	if (p[2] >= tapeBottom && p[2] < tapeBottom + 7.0f)
	{
		depth = tExit;
		const int row = (int)(p[2] - tapeBottom); // 0..6, bottom to top
		Uint8 color = _look.tapeBlack;
		if (row > 0 && row < 6)
		{
			const int u = ((int)std::floor(along) % (TapeTextLength * 4) + TapeTextLength * 4) % (TapeTextLength * 4);
			const int glyph = TapeText[u / 4], column = u % 4;
			const bool ink = column < 3 && (TapeFont[glyph][5 - row] & (4 >> column));
			color = ink ? _look.tapeBlack : _look.tapeYellow;
		}
		return shadeColor(color, (int)(shade + dither(px, py)));
	}
	if (d[2] < 0.0f && _ground)
	{
		const float tg = (GroundTop - o[2]) / d[2];
		if (tg >= tExit)
		{
			depth = tg;
			const float gx = o[0] + d[0] * tg, gy = o[1] + d[1] * tg;
			Uint8 color = _ground->color[1][(int)std::floor(gy) & 15][(int)std::floor(gx) & 15];
			if (!color) color = _ground->color[0][(int)std::floor(gy) & 15][(int)std::floor(gx) & 15];
			color = shadeColor(color, (int)(_look.ambient + _look.faceShade[4] + tg / 320.0f + dither(px, py)));
			const float haze = _look.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, tg)
				: smoothstep(_maxDistance * 0.3f, _maxDistance * 1.6f, tg);
			if (haze > dither(px + 2, py + 1))
			{
				color = _look.haze;
			}
			return color;
		}
	}
	depth = Infinity;
	return skyColor(d, px, py);
}

/**
 * The sky: a dithered gradient, clouds and the sun by day, stars and the moon by night,
 * murky water when the battle is under the sea.
 */
Uint8 FirstPersonView::skyColor(const float d[3], int px, int py) const
{
	const float e = std::max(d[2], 0.0f);
	const float gradient = std::pow(e, 0.6f) * 31.0f;
	Uint8 color = _look.sky[std::clamp((int)(gradient + dither(px, py)), 0, 31)];
	if (_look.underwater)
	{
		return color;
	}

	const float day = 1.0f - _look.night;
	const float sunDot = d[0] * _look.sunDir[0] + d[1] * _look.sunDir[1] + d[2] * _look.sunDir[2];
	if (day > 0.4f && sunDot > 0.9995f)
	{
		return _look.sun;
	}
	if (_look.night > 0.4f)
	{
		const float moonDot = d[0] * _look.moonDir[0] + d[1] * _look.moonDir[1] + d[2] * _look.moonDir[2];
		if (moonDot > 0.9992f)
		{
			return _look.moon;
		}
	}

	// clouds on a flat layer high up, thinning out towards the horizon
	if (e > 0.02f && _look.night < 0.9f)
	{
		const float k = 1.0f / std::max(e, 0.06f);
		const float cu = d[0] * k * 1.6f + _look.cloudTime * 0.02f;
		const float cv = d[1] * k * 1.6f;
		const float n = cloudNoise(cu, cv);
		const float cover = smoothstep(0.52f, 0.72f, n) * smoothstep(0.02f, 0.2f, e);
		if (cover > dither(px + 1, py + 3))
		{
			const float darkness = std::clamp((0.8f - n) * 10.0f, 0.0f, 7.0f);
			return _look.cloud[std::clamp((int)(darkness + dither(px, py)), 0, 7)];
		}
	}

	if (_look.night > 0.5f && e > 0.03f)
	{
		const float az = std::atan2(d[1], d[0]), el = std::asin(std::min(d[2], 1.0f));
		const Uint32 h = hash2((int)std::floor(az * 160.0f), (int)std::floor(el * 160.0f));
		if ((h % 1000) < 6)
		{
			return _look.star;
		}
	}
	return color;
}

/**
 * Ray casts every n-th row, starting at the given one.
 */
void FirstPersonView::renderRows(int first, int step)
{
	const int w = getWidth(), h = getHeight();
	const float halfW = (float)_fovTan;
	const float halfH = (float)(_fovTan * h / w);
	RayContext ctx;
	ctx.tiles = _tileViews.data();
	ctx.swings = _swings.data();
	ctx.size[0] = _save->getMapSizeX();
	ctx.size[1] = _save->getMapSizeY();
	ctx.size[2] = _save->getMapSizeZ();
	ctx.maxDistance = _maxDistance;
	const float o[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
	const float fwd[3] = { (float)_fwd[0], (float)_fwd[1], (float)_fwd[2] };
	const float right[3] = { (float)_right[0], (float)_right[1], (float)_right[2] };
	const float up[3] = { (float)_up[0], (float)_up[1], (float)_up[2] };

	// with a pixel size above 1, one ray colours a block of pixels
	const int k = std::clamp(Options::fpsPixelSize, 1, 3);
	const int blockRows = (h + k - 1) / k, blockCols = (w + k - 1) / k;
	for (int br = first; br < blockRows; br += step)
	{
		const int row = br * k;
		const float v = (1.0f - (row + k * 0.5f) * 2.0f / h) * halfH;
		for (int bc = 0; bc < blockCols; ++bc)
		{
			const int col = bc * k;
			const float u = ((col + k * 0.5f) * 2.0f / w - 1.0f) * halfW;
			float d[3];
			for (int a = 0; a < 3; ++a)
			{
				d[a] = fwd[a] + u * right[a] + v * up[a];
			}
			d[2] /= ZScaleF; // into the voxel grid (t stays along this ray; z below is the true depth)
			const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			for (int a = 0; a < 3; ++a)
			{
				d[a] /= len;
			}
			Hit hit;
			float tEnd, t;
			int endAxis;
			Uint8 color;
			switch (castRay(ctx, o, d, hit, tEnd, endAxis))
			{
			case RAY_HIT:
				color = shadeHit(hit, o, d, bc, br);
				t = hit.t;
				break;
			case RAY_SIDE:
				color = outsideColor(o, d, tEnd, endAxis, bc, br, t);
				break;
			case RAY_FAR:
				color = _look.haze;
				t = Infinity;
				break;
			case RAY_BOTTOM:
				color = 0x0F;
				t = Infinity;
				break;
			default:
				// out through the top: the ground below the map's edge can still be in sight
				color = outsideColor(o, d, tEnd, 2, bc, br, t);
				break;
			}
			if (hit.glass)
			{
				color = _look.glassTint[color];
			}
			const Uint8 glowing = (hit.emissive && t < Infinity) ? 1 : 0;
			const float z = t / len; // distance along the view axis, for sprite sorting
			for (int y = row; y < std::min(h, row + k); ++y)
			{
				Uint8 *dest = getRaw(0, y);
				float *depth = &_depth[y * w];
				for (int x = col; x < std::min(w, col + k); ++x)
				{
					_glowMask[y * w + x] = glowing;
					dest[x] = color;
					depth[x] = z;
				}
			}
		}
	}
}

/**
 * Projects a point in voxels to the view.
 * @return False if it's behind the camera.
 */
bool FirstPersonView::project(const double world[3], double &sx, double &sy, double &depth) const
{
	double rel[3], zc = 0.0, xc = 0.0, yc = 0.0;
	for (int a = 0; a < 3; ++a)
	{
		rel[a] = (world[a] - _eye[a]) * (a == 2 ? ZScale : 1.0);
		zc += rel[a] * _fwd[a];
		xc += rel[a] * _right[a];
		yc += rel[a] * _up[a];
	}
	if (zc < 4.0)
	{
		return false;
	}
	const double focal = (getWidth() / 2.0) / _fovTan;
	sx = getWidth() / 2.0 + xc * focal / zc;
	sy = getHeight() / 2.0 - yc * focal / zc;
	depth = zc;
	return true;
}

/**
 * Draws a sprite standing in the world: its anchor pixel at a point in voxels,
 * one sprite pixel per voxel (times scale), depth-tested against what's already drawn.
 */
void FirstPersonView::drawSprite(const Surface *src, int anchorX, int anchorY, const double world[3], double depthBias, int shade, double scale)
{
	double sx, sy, zc;
	if (!src || !project(world, sx, sy, zc))
	{
		return;
	}
	const int w = getWidth(), h = getHeight();
	// screen pixels per sprite pixel: isometric pixels are 1/sqrt(2) of a voxel across and one voxel up
	const double pixelsX = (w / 2.0) / _fovTan / zc * scale * SpritePixelX;
	const double pixelsY = (w / 2.0) / _fovTan / zc * scale * ZScale;
	const int sw = src->getWidth(), sh = src->getHeight();
	const int x0 = std::max(0, (int)std::floor(sx - anchorX * pixelsX));
	const int x1 = std::min(w, (int)std::ceil(sx + (sw - anchorX) * pixelsX));
	const int y0 = std::max(0, (int)std::floor(sy - anchorY * pixelsY));
	const int y1 = std::min(h, (int)std::ceil(sy + (sh - anchorY) * pixelsY));
	const float depth = (float)std::max(1.0, zc - depthBias);
	// right in front of the camera a sprite is a wall of huge pixels: fade it out (dithered),
	// but not a unit on the next tile (melee range), which stays solid
	const float opacity = (float)std::clamp((zc - 4.0) / 6.0, 0.0, 1.0);
	if (opacity <= 0.0f)
	{
		return;
	}
	if (x1 <= x0 || y1 <= y0)
	{
		return;
	}
	// which sprite column each screen column shows, worked out once (close up a sprite covers
	// much of the screen); rows are read straight from the sprite
	_spriteColumns.resize(x1 - x0);
	for (int x = x0; x < x1; ++x)
	{
		const int bx = (int)std::floor(anchorX + (x + 0.5 - sx) / pixelsX);
		_spriteColumns[x - x0] = bx >= 0 && bx < sw ? bx : -1;
	}
	const SDL_Surface *pixels = const_cast<Surface*>(src)->getSurface(); // read only
	for (int y = y0; y < y1; ++y)
	{
		const int by = (int)std::floor(anchorY + (y + 0.5 - sy) / pixelsY);
		if (by < 0 || by >= sh) continue;
		const Uint8 *row = (const Uint8 *)pixels->pixels + by * pixels->pitch;
		Uint8 *out = getRaw(0, y);
		float *depthRow = &_depth[y * w];
		for (int x = x0; x < x1; ++x)
		{
			const int bx = _spriteColumns[x - x0];
			if (bx < 0) continue;
			const Uint8 color = row[bx];
			if (!color || depth >= depthRow[x] || (opacity < 1.0f && dither(x, y) >= opacity)) continue;
			out[x] = shadeColor(color, shade);
			depthRow[x] = depth;
		}
	}
}

/**
 * Watches the units for hits and kills: a hit unit flinches back from the camera and flashes,
 * spraying blood (or sparks from machines); an enemy hit or killed in sight marks the
 * crosshair.
 */
void FirstPersonView::watchUnits()
{
	typedef FirstPersonParticles P;
	const Uint32 now = SDL_GetTicks();
	for (BattleUnit *unit : *_save->getUnits())
	{
		const bool out = unit->isOut();
		auto it = _watch.find(unit);
		if (it == _watch.end())
		{
			_watch.emplace(unit, UnitWatch{ unit->getHealth(), out, 0, { 0.0f, 0.0f } });
			continue;
		}
		UnitWatch &watch = it->second;
		const bool hit = unit->getHealth() < watch.health;
		const bool killed = out && !watch.out;
		watch.health = unit->getHealth();
		watch.out = out;
		if (!hit && !killed)
		{
			continue;
		}
		const Position pos = unit->getPosition();
		const int size = unit->getArmor()->getSize();
		const float at[3] = { pos.x * 16.0f + 8.0f * size, pos.y * 16.0f + 8.0f * size, pos.z * 24.0f + unit->getFloatHeight() + unit->getHeight() * 0.6f };
		const float dx = at[0] - (float)_eye[0], dy = at[1] - (float)_eye[1];
		const float flat = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
		if (hit)
		{
			watch.hitTick = now;
			watch.push[0] = dx / flat;
			watch.push[1] = dy / flat;
		}
		const bool seen = unit->getVisible() || _save->getDebugMode();
		if (seen && unit != _cameraUnit)
		{
			// blood or sparks, thrown away from the camera
			const bool flesh = unit->isWoundable();
			const Uint8 colors[2] = { flesh ? _look.flareFire[0] : _look.spark, flesh ? shadeColor(_look.flareFire[0], 5) : _look.fire[1] };
			for (int i = 0; i < 14; ++i)
			{
				P::Particle p;
				std::copy(at, at + 3, p.pos);
				const float speed = _particles.random(20, 60);
				p.vel[0] = dx / flat * speed + _particles.random(-15, 15);
				p.vel[1] = dy / flat * speed + _particles.random(-15, 15);
				p.vel[2] = _particles.random(-5, 30);
				p.age = 0; p.life = _particles.random(0.3f, 0.8f); p.gravity = 160; p.drag = 0.8f;
				p.color = colors[i & 1]; p.size = 2; p.kind = flesh ? P::LIT : P::GLOW; p.group = P::EFFECT;
				_particles.add(p);
			}
		}
		if (seen && unit->getFaction() == FACTION_HOSTILE)
		{
			_hitMarkTick = now;
			_hitMarkKill = killed;
		}
	}
}

/**
 * The game's shade where the camera is (0 bright .. 15 dark).
 */
int FirstPersonView::getEyeShade() const
{
	// before the first frame there's no snapshot of the tiles to read the light from
	if (!_save || (int)_tileViews.size() != _save->getMapSizeXYZ())
	{
		return 0;
	}
	return (int)lightAt((float)_eye[0], (float)_eye[1], (float)_eye[2]);
}

/**
 * The stereo angle of a sound from a tile, with this camera as the listener: 0 straight
 * ahead, 90 to the right, 180 behind, 270 to the left (for Mix_SetPosition).
 */
int FirstPersonView::soundAngle(const Position &tile) const
{
	const double dx = tile.x * 16.0 + 8.0 - _eye[0], dy = tile.y * 16.0 + 8.0 - _eye[1];
	const double dz = (tile.z * 24.0 + 12.0 - _eye[2]) * ZScale; // a level is 24 voxels up, a voxel up √(2/3) of one across
	if (dx * dx + dy * dy < 12.0 * 12.0 && std::abs(dz) < 12.0)
	{
		return 0; // right here: both ears, full volume
	}
	double angle = (std::atan2(dy, dx) - _yaw) * 180.0 / Pi;
	angle = std::fmod(angle + 720.0, 360.0);
	// quieter with distance: full within two tiles, fading over the next twenty-odd; and a little
	// quieter behind you (stereo alone can't tell front from back)
	const double tiles = std::sqrt(dx * dx + dy * dy + dz * dz) / 16.0;
	double distance = std::max(0.0, tiles - 2.0) * 9.0;
	const double behind = 1.0 - std::abs(angle - 180.0) / 70.0;
	if (behind > 0.0) distance += 30.0 * behind;
	return Sound::packDistance((int)angle, (int)std::min(220.0, distance));
}

/**
 * Draws visible units as billboards, using the normal unit sprites rotated to show the
 * side facing the camera; walking units glide between tiles.
 */
void FirstPersonView::drawUnits()
{
	const int animFrame = (SDL_GetTicks() / 100) % 8;
	UnitSprite unitSprite(_spriteBuffer, _game->getMod(), _save, animFrame, _save->getDepth() != 0, 0, 0);
	const bool debug = _save->getDebugMode();
	_markerSpots.clear();
	const Uint32 now = SDL_GetTicks();
	const double dt = _unitTick ? std::min<Uint32>(100, now - _unitTick) / 1000.0 : 0.0;
	_unitTick = now;

	for (BattleUnit *unit : *_save->getUnits())
	{
		if (unit->isOut() || unit == _cameraUnit || (!debug && !unit->getVisible()))
		{
			continue;
		}
		const int size = unit->getArmor()->getSize();
		double feet[3];
		auto place = [&](Position pos, double out[3]) {
			const Tile *tile = _save->getTile(pos);
			out[0] = pos.x * 16.0 + 8.0 * size;
			out[1] = pos.y * 16.0 + 8.0 * size;
			out[2] = pos.z * 24.0 - (tile ? tile->getTerrainLevel() : 0);
		};
		if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
		{
			double a[3], b[3];
			place(unit->getLastPosition(), a);
			place(unit->getDestination(), b);
			const int dir = unit->getDirection();
			const double phase = unit->getWalkingPhase() + unit->getDiagonalWalkingPhase();
			const double t = std::clamp(phase / ((dir % 2) ? 16.0 : 8.0), 0.0, 1.0);
			for (int i = 0; i < 3; ++i) feet[i] = a[i] + (b[i] - a[i]) * t;
		}
		else
		{
			place(unit->getPosition(), feet);
		}
		// the game moves units in small steps a few times a second: glide between them
		// (snap on anything big: a unit seen anew, teleported, falling a floor)
		Glide &glide = _glides[unit];
		const double jump = std::abs(glide.pos[0] - feet[0]) + std::abs(glide.pos[1] - feet[1]) + std::abs(glide.pos[2] - feet[2]);
		if (now - glide.seen > 250 || jump > 24.0)
		{
			std::copy(feet, feet + 3, glide.pos);
		}
		else
		{
			const double follow = std::min(1.0, dt * 18.0);
			for (int i = 0; i < 3; ++i)
			{
				glide.pos[i] += (feet[i] - glide.pos[i]) * follow;
				feet[i] = glide.pos[i];
			}
		}
		glide.seen = now;
		// a hit unit flinches back from the camera for a moment, and flashes
		float flash = 0.0f;
		auto watched = _watch.find(unit);
		if (watched != _watch.end() && watched->second.hitTick && now - watched->second.hitTick < 220)
		{
			const float d = 1.0f - (now - watched->second.hitTick) / 220.0f;
			feet[0] += watched->second.push[0] * 3.0 * d;
			feet[1] += watched->second.push[1] * 3.0 * d;
			flash = Options::fpsMuzzleFlash ? d : 0.0f;
		}

		for (const auto &marker : _markers)
		{
			if (marker.first == unit)
			{
				MarkerSpot spot = { { feet[0], feet[1], feet[2] + unit->getFloatHeight() + unit->getHeight() + 5.0 }, marker.second };
				_markerSpots.push_back(spot);
			}
		}

		// The isometric sprites show units as seen from the south-east (direction 3).
		// Rotate the unit's facing so the side turned towards the camera is shown.
		const double toCamera = std::atan2(_eye[1] - feet[1], _eye[0] - feet[0]) * 180.0 / Pi;
		const int cameraDir = (((int)std::lround((toCamera + 90.0) / 45.0)) % 8 + 8) % 8;
		unitSprite.setViewRotation(3 - cameraDir);

		_spriteBuffer->clear();
		const float chest[3] = { (float)feet[0], (float)feet[1], (float)feet[2] + 12.0f };
		const int shade = (int)std::lround(std::clamp(lightAt(chest[0], chest[1], chest[2]) - pointLight(chest, nullptr, false) - flareLight(chest, nullptr, false) * 8.0f - flash * 6.0f, 0.0f, 15.0f));
		if (Options::fpsShadows)
		{
			drawUnitShadow(feet, 5.5 * size); // flying units too: the shadow on the ground shows where they hover
		}
		const GraphSubset mask(SpriteBufferSize, SpriteBufferSize);
		// (drawn unlit and shaded when placed: an upscaled frame then serves in any light)
		if (size == 1)
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			drawSpriteSmooth(_spriteBuffer, 32, 40, feet, 0.0, shade); // anchored at the centre of the floor diamond
		}
		else
		{
			// parts are laid out on screen like the 2x2 tiles they cover, back to front
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
			unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
			unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
			drawSpriteSmooth(_spriteBuffer, 32, 48, feet, 0.0, shade); // anchored at the corner shared by the four tiles
		}
	}
}

/**
 * A soft dark patch on the ground under a unit, so it stands on the floor instead of floating.
 */
void FirstPersonView::drawUnitShadow(const double feet[3], double radius)
{
	drawGroundShade(feet, radius, 2, 1.8f);
}

/**
 * Darkens the ground in a soft disc (unit shadows, scorched earth): pixels whose floor lies
 * within the disc lose up to `steps` shades, thinning out (dithered) towards the edge.
 */
void FirstPersonView::drawGroundShade(const double feet[3], double radius, int steps, float density)
{
	const int w = getWidth(), h = getHeight();
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double world[3] = { feet[0] + ((corner & 1) ? radius : -radius), feet[1] + ((corner & 2) ? radius : -radius), feet[2] + 0.5 };
		double sx, sy, zc;
		if (!project(world, sx, sy, zc))
		{
			return;
		}
		x0 = std::min(x0, (int)sx); x1 = std::max(x1, (int)sx + 1);
		y0 = std::min(y0, (int)sy); y1 = std::max(y1, (int)sy + 1);
	}
	x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
	const double planeZ = feet[2] + 0.5;
	for (int y = y0; y <= y1; ++y)
	{
		Uint8 *row = getRaw(0, y);
		for (int x = x0; x <= x1; ++x)
		{
			double dir[3];
			getRayDirection(x, y, dir);
			if (dir[2] > -1e-4) continue;
			const double t = (planeZ - _eye[2]) / dir[2];
			if (t <= 0.0) continue;
			const double wx = _eye[0] + dir[0] * t - feet[0], wy = _eye[1] + dir[1] * t - feet[1];
			const double r = std::sqrt(wx * wx + wy * wy);
			if (r > radius) continue;
			const double zc = t * (dir[0] * _fwd[0] + dir[1] * _fwd[1] + dir[2] * ZScale * _fwd[2]); // true depth
			if (std::abs(_depth[y * w + x] - zc) > 3.0) continue; // only on the floor right there
			const float f = (float)(1.0 - r / radius);
			if (f * density > dither(x, y) && row[x])
			{
				row[x] = (Uint8)((row[x] & 0xF0) | std::min(15, (row[x] & 15) + steps));
			}
		}
	}
}

/**
 * Draws the top item lying on each discovered tile (weapons, corpses, ...).
 */
void FirstPersonView::drawFloorItems()
{
	const int animFrame = (SDL_GetTicks() / 100) % 8;
	ItemSprite itemSprite(_spriteBuffer, _game->getMod(), _save, animFrame);
	const int count = _save->getMapSizeXYZ();
	for (int i = 0; i < count; ++i)
	{
		Tile *tile = _save->getTile(i);
		if (tile->getInventory()->empty() || !_tileViews[i].discovered)
		{
			continue;
		}
		BattleItem *item = tile->getTopItem();
		if (!item)
		{
			continue;
		}
		const Position pos = tile->getPosition();
		const double ground[3] = { pos.x * 16.0 + 8.0, pos.y * 16.0 + 8.0, pos.z * 24.0 - tile->getTerrainLevel() };
		double sx, sy, zc;
		if (!project(ground, sx, sy, zc) || sx < -200 || sx > getWidth() + 200)
		{
			continue;
		}
		_spriteBuffer->clear();
		const int shade = (int)std::lround(std::clamp(lightAt((float)ground[0], (float)ground[1], (float)ground[2] + 4.0f), 0.0f, 15.0f));
		itemSprite.draw(item, 16, 8, 0);
		drawSpriteSmooth(_spriteBuffer, 32, 40, ground, 0.0, shade);
	}
}

/**
 * Draws the flying projectile (bullet trail or thrown item) and explosions.
 */
void FirstPersonView::drawEffects()
{
	if (!_map)
	{
		return;
	}
	Mod *mod = _game->getMod();
	if (Projectile *projectile = _map->getProjectile())
	{
		if (BattleItem *item = projectile->getItem())
		{
			const Position p = projectile->getPosition();
			const double world[3] = { (double)p.x, (double)p.y, (double)p.z };
			ItemSprite itemSprite(_spriteBuffer, mod, _save, 0);
			_spriteBuffer->clear();
			itemSprite.draw(item, 16, 8, 0);
			drawSpriteSmooth(_spriteBuffer, 32, 34, world, 4.0, 0); // like Map: the voxel sits at sprite pixel (16, 26)
		}
		else if (_trail != TRAIL_BEAM) // beams are drawn as lines
		{
			// the head of the shot: a small dot for bullets, a glowing blob for plasma, a flame for rockets
			float head[3];
			shotHead(projectile, head);
			drawShotDot(head, _trail, _trailColor);
		}
	}
	if (_map->getPartnerProjectile() && _partnerTrail != TRAIL_BEAM)
	{
		drawShotDot(_partnerHead, _partnerTrail, _partnerColor); // dual wield: the other gun's round
	}
	// explosions step through their frames on the game's clock; each frame dissolves into the
	// next over the time frames have been lasting
	const Uint32 now = SDL_GetTicks();
	for (auto it = _explosionClocks.begin(); it != _explosionClocks.end(); )
	{
		const auto &all = *_map->getExplosions();
		it = std::find(all.begin(), all.end(), it->first) == all.end() ? _explosionClocks.erase(it) : std::next(it);
	}
	for (const Explosion *explosion : *_map->getExplosions())
	{
		const int frame = explosion->getCurrentFrame();
		if (frame < 0)
		{
			continue;
		}
		FrameClock &clock = _explosionClocks.try_emplace(explosion, FrameClock{ frame, now, 0 }).first->second;
		if (frame != clock.frame)
		{
			clock.length = std::clamp<Uint32>(now - clock.since, 15, 250);
			clock.frame = frame;
			clock.since = now;
		}
		const double blend = clock.length ? std::min(1.0, (now - clock.since) / (double)clock.length) : 0.0;
		const Position p = explosion->getPosition();
		const double world[3] = { (double)p.x, (double)p.y, (double)p.z };
		SurfaceSet *set = mod->getSurfaceSet(explosion->isBig() ? "X1.PCK" : explosion->isHit() ? "HIT.PCK" : "SMOKE.PCK");
		const Surface *src = set->getFrame(frame);
		if (!src)
		{
			continue;
		}
		const Surface *next = frame + 1 < (int)set->getTotalFrames() ? set->getFrame(frame + 1) : nullptr;
		if (explosion->isBig())
		{
			drawSpriteBlend(src, next, blend, src->getWidth() / 2, src->getHeight() / 2, world, 24.0, 0);
		}
		else
		{
			drawSpriteBlend(src, next, blend, 15, explosion->isHit() ? 25 : 15, world, 12.0, 0);
		}
	}
}

void FirstPersonView::drawSpriteBlend(const Surface *from, const Surface *to, double blend, int anchorX, int anchorY, const double world[3], double depthBias, int shade)
{
	if (!from)
	{
		return;
	}
	if (!to || blend <= 0.0 || to->getWidth() != from->getWidth() || to->getHeight() != from->getHeight())
	{
		drawSpriteSmooth(from, anchorX, anchorY, world, depthBias, shade);
		return;
	}
	// close up both frames are upscaled first (the boxes are measured on the originals)
	SpriteBox a = spriteBox(from), b = spriteBox(to);
	double scale = 1.0;
	const Surface *bigFrom = upscaleHere(world, 1.0) ? upscaled(from) : nullptr;
	const Surface *bigTo = bigFrom ? upscaled(to) : nullptr;
	if (bigFrom && bigTo)
	{
		const int f = UpscaleFactor;
		from = bigFrom;
		to = bigTo;
		anchorX *= f;
		anchorY *= f;
		scale = 1.0 / f;
		for (SpriteBox *box : { &a, &b })
		{
			box->x0 *= f; box->y0 *= f;
			box->x1 = box->x1 * f + f - 1; box->y1 = box->y1 * f + f - 1;
		}
	}
	const int w = from->getWidth(), h = from->getHeight();
	Surface *buffer = nullptr;
	for (Surface *candidate : _blendBuffers)
	{
		if (candidate->getWidth() == w && candidate->getHeight() == h) buffer = candidate;
	}
	if (!buffer)
	{
		buffer = new Surface(w, h);
		_blendBuffers.push_back(buffer);
	}
	// a 4x4 ordered dither in sprite space: the pattern travels with the sprite, so it doesn't crawl
	static const int Bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	const int level = (int)(blend * 16.0);
	// and the shapes meet half way: both frames stretched to the size in between, around the base
	// they rise from, so a growing explosion is one shape growing, not two ghosting into each other
	const SDL_Surface *fromPixels = const_cast<Surface*>(from)->getSurface(), *toPixels = const_cast<Surface*>(to)->getSurface(); // read only
	SDL_Surface *out = buffer->getSurface();
	auto pixel = [](const SDL_Surface *surface, int x, int y) { return ((const Uint8 *)surface->pixels)[y * surface->pitch + x]; };
	if (!a.valid || !b.valid)
	{
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				((Uint8 *)out->pixels)[y * out->pitch + x] = Bayer[y & 3][x & 3] < level ? pixel(toPixels, x, y) : pixel(fromPixels, x, y);
		drawSprite(buffer, anchorX, anchorY, world, depthBias, shade, scale);
		return;
	}
	const double t = blend;
	const double wa = a.x1 - a.x0 + 1.0, ha = a.y1 - a.y0 + 1.0, wb = b.x1 - b.x0 + 1.0, hb = b.y1 - b.y0 + 1.0;
	const double wt = wa + (wb - wa) * t, ht = ha + (hb - ha) * t;
	const double baseA[2] = { (a.x0 + a.x1 + 1) / 2.0, a.y1 + 1.0 }, baseB[2] = { (b.x0 + b.x1 + 1) / 2.0, b.y1 + 1.0 };
	const double base[2] = { baseA[0] + (baseB[0] - baseA[0]) * t, baseA[1] + (baseB[1] - baseA[1]) * t };
	// where each output column and row reads from in either frame (-1: outside it); only the box
	// the stretched shapes can reach is worked out, the rest of the buffer is cleared
	_blendColumns.resize(4 * (size_t)std::max(w, h));
	int *colA = _blendColumns.data(), *colB = colA + w, *rowA = colB + w, *rowB = rowA + h;
	auto source = [](double origin, double at, double middle, double stretch, int size) {
		const int index = (int)std::floor(origin + (at + 0.5 - middle) / stretch);
		return index >= 0 && index < size ? index : -1;
	};
	int x0 = w, x1 = -1, y0 = h, y1 = -1;
	for (int x = 0; x < w; ++x)
	{
		colA[x] = source(baseA[0], x, base[0], wt / wa, w);
		colB[x] = source(baseB[0], x, base[0], wt / wb, w);
		if ((colA[x] >= a.x0 && colA[x] <= a.x1) || (colB[x] >= b.x0 && colB[x] <= b.x1)) { x0 = std::min(x0, x); x1 = x; }
	}
	for (int y = 0; y < h; ++y)
	{
		rowA[y] = source(baseA[1], y, base[1], ht / ha, h);
		rowB[y] = source(baseB[1], y, base[1], ht / hb, h);
		if ((rowA[y] >= a.y0 && rowA[y] <= a.y1) || (rowB[y] >= b.y0 && rowB[y] <= b.y1)) { y0 = std::min(y0, y); y1 = y; }
	}
	for (int y = 0; y < h; ++y)
	{
		Uint8 *row = (Uint8 *)out->pixels + y * out->pitch;
		if (y < y0 || y > y1)
		{
			std::fill(row, row + w, (Uint8)0);
			continue;
		}
		std::fill(row, row + std::max(0, x0), (Uint8)0);
		if (x1 + 1 < w) std::fill(row + x1 + 1, row + w, (Uint8)0);
		const Uint8 *lineA = rowA[y] >= 0 ? (const Uint8 *)fromPixels->pixels + rowA[y] * fromPixels->pitch : nullptr;
		const Uint8 *lineB = rowB[y] >= 0 ? (const Uint8 *)toPixels->pixels + rowB[y] * toPixels->pitch : nullptr;
		const int *dither = Bayer[y & 3];
		for (int x = x0; x <= x1; ++x)
		{
			if (dither[x & 3] < level) row[x] = lineB && colB[x] >= 0 ? lineB[colB[x]] : 0;
			else row[x] = lineA && colA[x] >= 0 ? lineA[colA[x]] : 0;
		}
	}
	drawSprite(buffer, anchorX, anchorY, world, depthBias, shade, scale);
}

/**
 * Is a sprite at this point big enough on screen to be worth upscaling (option)?
 */
bool FirstPersonView::upscaleHere(const double world[3], double scale) const
{
	if (Options::fpsSpriteUpscale <= 0)
	{
		return false;
	}
	double sx, sy, zc;
	if (!project(world, sx, sy, zc))
	{
		return false;
	}
	return (getWidth() / 2.0) / _fovTan / zc * scale * SpritePixelX >= UpscaleFrom;
}

/**
 * Draws a sprite, upscaled when it's close (see upscaleHere).
 */
void FirstPersonView::drawSpriteSmooth(const Surface *src, int anchorX, int anchorY, const double world[3], double depthBias, int shade)
{
	if (src && upscaleHere(world, 1.0))
	{
		if (const Surface *big = upscaled(src))
		{
			drawSprite(big, anchorX * UpscaleFactor, anchorY * UpscaleFactor, world, depthBias, shade, 1.0 / UpscaleFactor);
			return;
		}
	}
	drawSprite(src, anchorX, anchorY, world, depthBias, shade);
}

/**
 * A sprite frame made bigger the way pixel art scales well (setting "Sprite upscaling"): xBRZ
 * rounds the stair-stepped edges along the shapes the artist drew, without blurring; the result
 * goes back into the game's palette (each colour to its nearest), and with "Smooth + detail" the
 * shading is sharpened a little first. The work is done on a worker thread: until a frame is
 * ready this returns null and the original is drawn; ready frames are cached by their pixels.
 */
const Surface *FirstPersonView::upscaled(const Surface *src)
{
	const SDL_Surface *in = const_cast<Surface*>(src)->getSurface(); // read only
	const int w = in->w, h = in->h, f = UpscaleFactor;
	const bool detail = Options::fpsSpriteUpscale >= 2;
	if (_upscalePalette != _look.paletteHash)
	{
		// a new palette: everything made for the old one goes
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		for (auto &entry : _upscaled) delete entry.second.surface;
		_upscaled.clear();
		_upscaleDone.clear();
		_upscaleQueued.clear();
		_upscaleJobs.clear();
		_upscalePalette = _look.paletteHash;
	}
	Uint64 key = 1469598103934665603ULL ^ ((Uint64)w << 32) ^ ((Uint64)h << 48) ^ (detail ? 1 : 0);
	for (int y = 0; y < h; ++y)
	{
		const Uint8 *row = (const Uint8 *)in->pixels + y * in->pitch;
		for (int x = 0; x < w; ++x) key = (key ^ row[x]) * 1099511628211ULL;
	}
	auto it = _upscaled.find(key);
	if (it != _upscaled.end())
	{
		it->second.used = ++_upscaleClock;
		return it->second.surface;
	}
	std::vector<Uint8> done;
	{
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		auto ready = _upscaleDone.find(key);
		if (ready != _upscaleDone.end())
		{
			done.swap(ready->second);
			_upscaleDone.erase(ready);
			_upscaleQueued.erase(key);
		}
		else
		{
			if (!_upscaleQueued.count(key) && _upscaleJobs.size() < 64)
			{
				UpscaleJob job;
				job.key = key; job.w = w; job.h = h; job.detail = detail; job.paletteHash = _look.paletteHash;
				job.pixels.resize(w * h);
				for (int y = 0; y < h; ++y) std::copy((const Uint8 *)in->pixels + y * in->pitch, (const Uint8 *)in->pixels + y * in->pitch + w, job.pixels.begin() + y * w);
				const SDL_Color *pal = getPalette();
				job.palette.assign(pal, pal + 256);
				_upscaleJobs.push_back(std::move(job));
				_upscaleQueued.insert(key);
				if (!_upscaleThread.joinable()) _upscaleThread = std::thread(&FirstPersonView::upscaleWorker, this);
				_upscaleWake.notify_one();
			}
			return nullptr;
		}
	}
	Surface *result = new Surface(w * f, h * f);
	SDL_Surface *out = result->getSurface();
	for (int y = 0; y < h * f; ++y) std::copy(done.begin() + y * w * f, done.begin() + (y + 1) * w * f, (Uint8 *)out->pixels + y * out->pitch);
	// a few hundred frames are kept; the one unused longest goes
	if (_upscaled.size() >= 400)
	{
		auto oldest = _upscaled.begin();
		for (auto e = _upscaled.begin(); e != _upscaled.end(); ++e)
		{
			if (e->second.used < oldest->second.used) oldest = e;
		}
		delete oldest->second.surface;
		_upscaled.erase(oldest);
	}
	_upscaled.emplace(key, Upscaled{ result, ++_upscaleClock });
	return result;
}

/**
 * The upscaling worker: takes frames off the queue and makes their bigger versions (see
 * upscaled), with its own nearest-colour table for the palette they came with.
 */
void FirstPersonView::upscaleWorker()
{
#ifdef __linux__
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15); // spare time only: the frame's threads come first
#endif
	std::vector<Uint8> lut;
	Uint64 lutHash = 0;
	while (true)
	{
		UpscaleJob job;
		{
			std::unique_lock<std::mutex> lock(_upscaleMutex);
			_upscaleWake.wait(lock, [this] { return _upscaleStop || !_upscaleJobs.empty(); });
			if (_upscaleStop) return;
			job = std::move(_upscaleJobs.front());
			_upscaleJobs.pop_front();
		}
		const std::vector<SDL_Color> &pal = job.palette;
		if (lut.empty() || lutHash != job.paletteHash)
		{
			lut.assign(32768, 0);
			for (int c = 0; c < 32768; ++c)
			{
				const int r = ((c >> 10) & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = (c & 31) * 255 / 31;
				int best = 1, bestDist = INT_MAX;
				for (int i = 1; i < 256; ++i)
				{
					const int dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
					const int dist = dr * dr * 3 + dg * dg * 4 + db * db * 2;
					if (dist < bestDist) { bestDist = dist; best = i; }
				}
				lut[c] = (Uint8)best;
			}
			lutHash = job.paletteHash;
		}
		const int w = job.w, h = job.h, f = UpscaleFactor, W = w * f, H = h * f;
		std::vector<uint32_t> argb(w * h), big(W * H);
		for (int i = 0; i < w * h; ++i)
		{
			const Uint8 c = job.pixels[i];
			argb[i] = c ? 0xFF000000u | ((uint32_t)pal[c].r << 16) | ((uint32_t)pal[c].g << 8) | pal[c].b : 0u;
		}
		xbrz::scale(f, argb.data(), big.data(), w, h, xbrz::ARGB);
		std::vector<float> rgb(W * H * 3);
		for (int i = 0; i < W * H; ++i)
		{
			const uint32_t v = big[i];
			const float a = (v >> 24) / 255.0f; // xBRZ blends edges with transparency: keep the colour, not the darkening
			for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = a > 0.0f ? ((v >> (16 - 8 * k)) & 255) / std::max(a, 0.5f) : 0.0f;
		}
		auto solid = [&](int i) { return (big[i] >> 24) >= 128 ? 1.0f : 0.0f; };
		if (job.detail)
		{
			// unsharp mask: the colour pushed away from a 5x5 average of the sprite around it (only
			// its own pixels count, so outlines don't get a rim of light)
			std::vector<float> tmp(W * H * 4), blur(W * H * 4);
			const int R = 2;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					float sum[4] = {};
					for (int d = -R; d <= R; ++d)
					{
						const int i = y * W + std::clamp(x + d, 0, W - 1);
						const float m = solid(i);
						for (int k = 0; k < 3; ++k) sum[k] += rgb[i * 3 + k] * m;
						sum[3] += m;
					}
					for (int k = 0; k < 4; ++k) tmp[(y * W + x) * 4 + k] = sum[k];
				}
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					float sum[4] = {};
					for (int d = -R; d <= R; ++d)
					{
						const int i = std::clamp(y + d, 0, H - 1) * W + x;
						for (int k = 0; k < 4; ++k) sum[k] += tmp[i * 4 + k];
					}
					for (int k = 0; k < 4; ++k) blur[(y * W + x) * 4 + k] = sum[k];
				}
			for (int i = 0; i < W * H; ++i)
			{
				if (!solid(i) || blur[i * 4 + 3] <= 0.0f) continue;
				for (int k = 0; k < 3; ++k)
				{
					const float diff = rgb[i * 3 + k] - blur[i * 4 + k] / blur[i * 4 + 3];
					if (std::abs(diff) > 3.0f) rgb[i * 3 + k] = std::clamp(rgb[i * 3 + k] + 0.7f * diff, 0.0f, 255.0f);
				}
			}
		}
		std::vector<Uint8> result(W * H, 0);
		for (int i = 0; i < W * H; ++i)
		{
			if (!solid(i)) continue;
			const int r = (int)std::clamp(rgb[i * 3], 0.0f, 255.0f), g = (int)std::clamp(rgb[i * 3 + 1], 0.0f, 255.0f), b = (int)std::clamp(rgb[i * 3 + 2], 0.0f, 255.0f);
			result[i] = lut[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
		}
		std::lock_guard<std::mutex> lock(_upscaleMutex);
		if (_upscaleQueued.count(job.key)) _upscaleDone[job.key] = std::move(result); // (dropped if the palette changed meanwhile)
	}
}

/**
 * The box round a sprite frame's pixels (measured once per frame).
 */
const FirstPersonView::SpriteBox &FirstPersonView::spriteBox(const Surface *sprite)
{
	auto it = _spriteBoxes.find(sprite);
	if (it != _spriteBoxes.end())
	{
		return it->second;
	}
	SpriteBox box = { sprite->getWidth(), sprite->getHeight(), -1, -1, false };
	for (int y = 0; y < sprite->getHeight(); ++y)
	{
		for (int x = 0; x < sprite->getWidth(); ++x)
		{
			if (!sprite->getPixel(x, y)) continue;
			box.x0 = std::min(box.x0, x); box.y0 = std::min(box.y0, y);
			box.x1 = std::max(box.x1, x); box.y1 = std::max(box.y1, y);
			box.valid = true;
		}
	}
	return _spriteBoxes.emplace(sprite, box).first->second;
}

/**
 * Smoke and fire on discovered tiles, animated like on the isometric map.
 */
void FirstPersonView::drawSmokeAndFire()
{
	SurfaceSet *set = _game->getMod()->getSurfaceSet("SMOKE.PCK", false);
	if (!set)
	{
		return;
	}
	// the four frames last 200 ms each like on the map; each dissolves into the next meanwhile
	const Uint32 now = SDL_GetTicks();
	const int halfAnimFrame = (now / 200) % 4;
	const double blend = (now % 200) / 200.0;
	const int count = _save->getMapSizeXYZ();
	for (int i = 0; i < count; ++i)
	{
		Tile *tile = _save->getTile(i);
		if (!tile->getSmoke() || !_tileViews[i].discovered)
		{
			continue;
		}
		int frame = 0, shade = 0;
		if (!tile->getFire())
		{
			frame += _save->getDepth() > 0 ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
			if (Mod::EXTENDED_SMOKE_OFFSET == 0)
				frame += int(std::floor((tile->getSmoke() / 6.0) - 0.1));
			else if (Mod::EXTENDED_SMOKE_OFFSET == 1)
				frame += int(std::floor((tile->getSmoke() / 6.0) - 0.1)) * 4;
			else
				frame += (tile->getSmoke() - 1) / 5 * 4;
			shade = _tileViews[i].light;
		}
		const int phase = (halfAnimFrame + tile->getAnimationOffset()) % 4;
		const Position pos = tile->getPosition();
		const double ground[3] = { pos.x * 16.0 + 8.0, pos.y * 16.0 + 8.0, pos.z * 24.0 - tile->getTerrainLevel() };
		drawSpriteBlend(set->getFrame(frame + phase), set->getFrame(frame + (phase + 1) % 4), blend, 16, 32, ground, 2.0, shade);
	}
}

/**
 * Is there open sky above this point (no floor on any level above it)?
 */
bool FirstPersonView::openSky(float x, float y, float z) const
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ();
	const int tx = (int)std::floor(x / 16), ty = (int)std::floor(y / 16);
	if (tx < 0 || ty < 0 || tx >= sx || ty >= sy)
	{
		return true;
	}
	for (int tz = std::max(0, (int)std::floor(z / 24) + 1); tz < sz; ++tz)
	{
		const TileView &tv = _tileViews[(tz * sy + ty) * sx + tx];
		if (tv.part[O_FLOOR] || tv.part[O_OBJECT])
		{
			return false;
		}
	}
	return true;
}

/**
 * Under the sea: is this point in open water? Not inside anything solid, and not under a roof
 * right above (inside the sub or a building); ruins towering further up don't matter.
 */
bool FirstPersonView::inWater(float x, float y, float z) const
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ();
	const int tx = (int)std::floor(x / 16), ty = (int)std::floor(y / 16), tz = (int)std::floor(z / 24);
	if (tx < 0 || ty < 0 || tx >= sx || ty >= sy || tz < 0 || tz >= sz)
	{
		return true;
	}
	const TileView &tv = _tileViews[(tz * sy + ty) * sx + tx];
	const int lx = std::clamp((int)x - tx * 16, 0, 15), ly = std::clamp((int)y - ty * 16, 0, 15), lz = std::clamp((int)z - tz * 24, 0, 23);
	for (const VoxelModel *m : tv.part)
	{
		if (m && m->color[lz][ly][lx]) return false;
	}
	if (tz + 1 < sz && _tileViews[((tz + 1) * sy + ty) * sx + tx].part[O_FLOOR])
	{
		return false;
	}
	return true;
}

/**
 * Colour of the terrain at (or right next to) a point, for debris; a neutral grey if none.
 */
Uint8 FirstPersonView::colorAt(const float pos[3]) const
{
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY(), sz = _save->getMapSizeZ();
	for (int r = 0; r <= 1; ++r)
	{
		for (int dz = -r; dz <= r; ++dz)
		for (int dy = -r; dy <= r; ++dy)
		for (int dx = -r; dx <= r; ++dx)
		{
			const int vx = (int)pos[0] + dx, vy = (int)pos[1] + dy, vz = (int)pos[2] + dz;
			const int tx = vx / 16, ty = vy / 16, tz = vz / 24;
			if (vx < 0 || vy < 0 || vz < 0 || tx >= sx || ty >= sy || tz >= sz)
			{
				continue;
			}
			const TileView &tv = _tileViews[(tz * sy + ty) * sx + tx];
			for (int part = 0; part < 4; ++part)
			{
				if (tv.part[part])
				{
					const Uint8 c = tv.part[part]->color[vz % 24][vy % 16][vx % 16];
					if (c) return c;
				}
			}
		}
	}
	return _look.smoke;
}

/**
 * Spawns particles for what happens in the battle: a muzzle flash and smoke when a shot
 * leaves, tracer sparks along its flight, sparks and debris (in the colour of what was hit)
 * where it lands, embers, flash and smoke from explosions, embers and smoke from burning
 * tiles, drifting smoke from smoky tiles.
 */
void FirstPersonView::spawnEffects(float dt)
{
	typedef FirstPersonParticles P;
	Projectile *projectile = _map ? _map->getProjectile() : nullptr;
	if (projectile != _lastProjectile)
	{
		if (_lastProjectile && !_lastProjectileThrown)
		{
			shotLanded(_lastHead, _trail);
		}
		if (_lastProjectile && _shotIsEnemy)
		{
			_enemyShotEnd = SDL_GetTicks(); // (a hit on our soldier right now came from _enemyShotFrom)
		}
		if (_lastProjectile && !_lastProjectileThrown && _trail == TRAIL_BEAM)
		{
			_beams.push_back({ { _trailOrigin[0], _trailOrigin[1], _trailOrigin[2] }, { _lastHead[0], _lastHead[1], _lastHead[2] }, SDL_GetTicks(), _trailColor });
		}
		if (projectile)
		{
			// muzzle: a quick bright flash towards the target and a little smoke
			const Position from = projectile->getPosition(-100000), next = projectile->getPosition(0);
			float origin[3] = { (float)from.x, (float)from.y, (float)from.z };
			_lastProjectileThrown = projectile->getItem() != nullptr;
			// our own shot leaves the weapon in hand: from its muzzle on the screen, a little ahead
			const double ox = origin[0] - _eye[0], oy = origin[1] - _eye[1], oz = origin[2] - _eye[2];
			const bool ownShot = !_lastProjectileThrown && ox * ox + oy * oy + oz * oz < 20.0 * 20.0;
			if (ownShot)
			{
				_ownShotTick = SDL_GetTicks();
			}
			_shotIsEnemy = !ownShot && !_lastProjectileThrown;
			_nearMissDone = !_shotIsEnemy;
			if (_shotIsEnemy)
			{
				for (int a = 0; a < 3; ++a) _enemyShotFrom[a] = origin[a];
			}
			const Position end = projectile->getPosition(100000);
			for (int a = 0; a < 3; ++a) _shotFrom[a] = origin[a];
			_shotLength = std::max(1.0f, (float)std::sqrt(Position::distanceSq(from, end)));
			std::fill(_shotOffset, _shotOffset + 3, 0.0f);
			if (ownShot && _muzzleValid)
			{
				double dir[3];
				getRayDirection((int)_muzzleScreen[0], (int)_muzzleScreen[1], dir);
				for (int a = 0; a < 3; ++a)
				{
					origin[a] = (float)(_eye[a] + dir[a] * 12.0);
					_shotOffset[a] = origin[a] - _shotFrom[a];
				}
			}
			std::copy(origin, origin + 3, _trailOrigin);
			_lastHead[0] = origin[0]; _lastHead[1] = origin[1]; _lastHead[2] = origin[2];
			// what the shot looks like in flight, from its damage type
			const BattleItem *ammo = projectile->getAmmo();
			if (ownShot)
			{
				_ownShotAmmo = ammo ? ammo->getRules() : nullptr;
			}
			_trail = TRAIL_NONE;
			_trailColor = _look.spark;
			if (!_lastProjectileThrown)
			{
				trailFor(ammo, _trail, _trailColor);
			}
			if (!_lastProjectileThrown)
			{
				// the flash lights up the room for a moment
				std::copy(origin, origin + 3, _muzzle);
				_muzzleTick = SDL_GetTicks();
				// our own shot kicks the view: hard for guns, by their power; lasers barely
				if (ownShot)
				{
					const float power = ammo ? (float)ammo->getRules()->getPower() : 30.0f;
					const float weight = _trail == TRAIL_BEAM ? 0.35f : _trail == TRAIL_PLASMA ? 0.7f : 1.0f;
					_kick = std::min(_kick + std::clamp(power / 22.0f, 0.8f, 3.5f) * weight, 6.0f);
					addShake(0.25f + 0.1f * weight);
				}
				float dir[3] = { (float)(next.x - from.x), (float)(next.y - from.y), (float)(next.z - from.z) };
				const float len = std::max(1.0f, std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]));
				// (our own shot's flash is drawn on the weapon in hand; sparks and smoke right before
				// the eyes would be a few huge squares)
				const bool atMuzzle = ownShot && _muzzleValid;
				for (int i = 0; i < (Options::fpsMuzzleFlash && !atMuzzle ? 12 : 0); ++i)
				{
					P::Particle p;
					const float speed = _particles.random(20, 70);
					for (int a = 0; a < 3; ++a)
					{
						p.pos[a] = origin[a];
						p.vel[a] = dir[a] / len * speed + _particles.random(-12, 12);
					}
					p.age = 0; p.life = _particles.random(0.05f, 0.12f); p.gravity = 0; p.drag = 4;
					p.color = _look.fire[(int)_particles.random(0, 2)]; p.size = 3; p.kind = P::GLOW; p.group = P::EFFECT;
					_particles.add(p);
				}
				const Uint8 smoke[1] = { _look.smoke };
				if (!atMuzzle) _particles.burst(origin, 4, P::LIT, P::EFFECT, smoke, 1, 2, 8, 0.6f, -8, 1.2f, 0.8f, 1.4f, 3);
			}
		}
		_lastProjectile = projectile;
	}
	if (projectile)
	{
		const float previous[3] = { _lastHead[0], _lastHead[1], _lastHead[2] };
		shotHead(projectile, _lastHead);
		trailSegment(previous, _lastHead);
		if (!_nearMissDone)
		{
			// an enemy shot passing close by the eye whizzes past (once per shot)
			float seg[3], rel[3], len2 = 0.0f, along = 0.0f;
			for (int a = 0; a < 3; ++a)
			{
				seg[a] = _lastHead[a] - previous[a];
				rel[a] = (float)_eye[a] - previous[a];
				len2 += seg[a] * seg[a];
				along += seg[a] * rel[a];
			}
			const float t = len2 > 0.0f ? std::clamp(along / len2, 0.0f, 1.0f) : 0.0f;
			float d2 = 0.0f, near[3];
			for (int a = 0; a < 3; ++a)
			{
				near[a] = previous[a] + seg[a] * t;
				d2 += (near[a] - (float)_eye[a]) * (near[a] - (float)_eye[a]);
			}
			if (d2 < 14.0f * 14.0f)
			{
				_nearMissDone = true;
				_nearMissTick = SDL_GetTicks();
				_nearMissDir[0] = near[0] - _eye[0];
				_nearMissDir[1] = near[1] - _eye[1];
			}
		}
	}

	// explosions
	std::set<const Explosion*> current;
	if (_map)
	{
		for (const Explosion *explosion : *_map->getExplosions())
		{
			current.insert(explosion);
			if (_seenExplosions.count(explosion))
			{
				continue;
			}
			const Position p = explosion->getPosition();
			const float pos[3] = { (float)p.x, (float)p.y, (float)p.z };
			const float ex = pos[0] - (float)_eye[0], ey = pos[1] - (float)_eye[1], ez = pos[2] - (float)_eye[2];
			const float near = 1.0f - std::sqrt(ex * ex + ey * ey + ez * ez) / (explosion->isBig() ? 260.0f : 90.0f);
			if (near > 0.0f)
			{
				addShake(near * (explosion->isBig() ? 3.0f : 1.0f));
			}
			if (explosion->isBig())
			{
				// the ground around it is blackened
				const float floorZ = std::floor(pos[2] / 24.0f) * 24.0f + GroundTop;
				if (pos[2] - floorZ < 18.0f && _scorches.size() < 60)
				{
					_scorches.push_back({ { pos[0], pos[1], floorZ }, 14.0f });
				}
				const Uint8 smoke[2] = { _look.smoke, shadeColor(_look.smoke, 3) };
				_particles.burst(pos, 18, P::GLOW, P::EFFECT, _look.fire, 2, 10, 40, 0, 0, 3, 0.08f, 0.2f, 6);
				_particles.burst(pos, 60, P::GLOW, P::EFFECT, _look.fire, 4, 40, 140, 0.5f, 150, 0.6f, 0.6f, 1.5f, 2);
				_particles.burst(pos, 26, P::LIT, P::EFFECT, smoke, 2, 6, 24, 0.7f, -12, 0.8f, 2.0f, 3.5f, 5);
			}
			else
			{
				const Uint8 sparks[2] = { _look.spark, _look.fire[1] };
				_particles.burst(pos, 8, P::GLOW, P::EFFECT, sparks, 2, 20, 60, 0.3f, 120, 1.0f, 0.15f, 0.4f, 2);
			}
		}
	}
	_seenExplosions.swap(current);

	// burning and smoky tiles near the camera
	const int count = _save->getMapSizeXYZ();
	for (int i = 0; i < count; ++i)
	{
		Tile *tile = _save->getTile(i);
		if ((!tile->getFire() && !tile->getSmoke()) || !_tileViews[i].discovered)
		{
			continue;
		}
		const Position t = tile->getPosition();
		const float cx = t.x * 16 + 8.0f, cy = t.y * 16 + 8.0f, cz = t.z * 24 - tile->getTerrainLevel() + 2.0f;
		if (std::abs(cx - _eye[0]) > 320 || std::abs(cy - _eye[1]) > 320)
		{
			continue;
		}
		if (tile->getFire() && _particles.random() < 10.0f * dt)
		{
			P::Particle p;
			p.pos[0] = cx + _particles.random(-6, 6); p.pos[1] = cy + _particles.random(-6, 6); p.pos[2] = cz + _particles.random(0, 10);
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(14, 34);
			p.age = 0; p.life = _particles.random(0.7f, 1.6f); p.gravity = -10; p.drag = 0.8f;
			p.color = _look.fire[(int)_particles.random(0, 4)]; p.size = 2; p.kind = P::GLOW; p.group = P::EFFECT;
			_particles.add(p);
		}
		if (tile->getSmoke() && _particles.random() < (tile->getFire() ? 3.0f : 1.5f + tile->getSmoke() * 0.2f) * dt)
		{
			P::Particle p;
			p.pos[0] = cx + _particles.random(-7, 7); p.pos[1] = cy + _particles.random(-7, 7); p.pos[2] = cz + _particles.random(6, 18);
			p.vel[0] = _particles.random(-3, 3) + 2; p.vel[1] = _particles.random(-3, 3); p.vel[2] = _particles.random(3, 9);
			p.age = 0; p.life = _particles.random(2.0f, 3.5f); p.gravity = -2; p.drag = 0.4f;
			p.color = shadeColor(_look.smoke, (int)_particles.random(0, 4)); p.size = 5; p.kind = P::LIT; p.group = P::EFFECT;
			_particles.add(p);
		}
	}

	// burning units: flames all over them, and smoke off the top
	const Uint32 now = SDL_GetTicks();
	for (const BattleUnit *unit : *_save->getUnits())
	{
		if (unit->getFire() <= 0 || unit->isOut() || unit == _cameraUnit || (!unit->getVisible() && !_save->getDebugMode()))
		{
			continue;
		}
		const int size = unit->getArmor()->getSize();
		float feet[3];
		auto glide = _glides.find(unit);
		if (glide != _glides.end() && now - glide->second.seen < 250)
		{
			for (int a = 0; a < 3; ++a) feet[a] = (float)glide->second.pos[a];
		}
		else
		{
			const Position t = unit->getPosition();
			const Tile *tile = _save->getTile(t);
			feet[0] = t.x * 16 + 8.0f * size; feet[1] = t.y * 16 + 8.0f * size; feet[2] = t.z * 24 - (tile ? tile->getTerrainLevel() : 0);
		}
		if (std::abs(feet[0] - _eye[0]) > 320 || std::abs(feet[1] - _eye[1]) > 320)
		{
			continue;
		}
		const float height = (float)(unit->getHeight() + unit->getFloatHeight());
		const int flames = (int)(dt * 45.0f * size + _particles.random());
		for (int k = 0; k < flames; ++k)
		{
			P::Particle p;
			p.pos[0] = feet[0] + _particles.random(-4, 4) * size; p.pos[1] = feet[1] + _particles.random(-4, 4) * size;
			p.pos[2] = feet[2] + height * _particles.random(0.05f, 0.9f);
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(16, 36);
			p.age = 0; p.life = _particles.random(0.35f, 0.8f); p.gravity = -12; p.drag = 0.8f;
			p.color = _look.fire[(int)_particles.random(0, 3.99f)]; p.size = 3; p.kind = P::GLOW; p.group = P::EFFECT;
			_particles.add(p);
		}
		if (_particles.random() < dt * 5.0f)
		{
			P::Particle p;
			p.pos[0] = feet[0] + _particles.random(-3, 3); p.pos[1] = feet[1] + _particles.random(-3, 3); p.pos[2] = feet[2] + height + 2.0f;
			p.vel[0] = _particles.random(-3, 3) + 2; p.vel[1] = _particles.random(-3, 3); p.vel[2] = _particles.random(6, 12);
			p.age = 0; p.life = _particles.random(1.5f, 2.5f); p.gravity = -3; p.drag = 0.4f;
			p.color = shadeColor(_look.smoke, (int)_particles.random(1, 4)); p.size = 5; p.kind = P::LIT; p.group = P::EFFECT;
			_particles.add(p);
		}
	}
}

/**
 * Throws a tactical flare: from the right shoulder, on an arc that comes down at the target
 * (it may hit a wall first, bounce and drop). Under the sea it's only a short, slow toss.
 */
void FirstPersonView::throwFlare(const float target[3])
{
	Flare flare;
	for (int a = 0; a < 3; ++a)
	{
		flare.pos[a] = (float)(_eye[a] + _right[a] * 3.0 - _up[a] * 2.0 + _fwd[a] * 3.0);
	}
	if (_look.underwater)
	{
		const double fx = _fwd[0], fy = _fwd[1], flat = std::max(1e-6, std::sqrt(fx * fx + fy * fy));
		flare.vel[0] = (float)(fx / flat * 28.0); flare.vel[1] = (float)(fy / flat * 28.0); flare.vel[2] = 6.0f;
	}
	else
	{
		float d[3] = { target[0] - flare.pos[0], target[1] - flare.pos[1], target[2] - flare.pos[2] };
		float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		const float reach = 160.0f; // ten tiles: a strong throw
		if (dist > reach)
		{
			for (float &a : d) a *= reach / dist;
			dist = reach;
		}
		const float time = std::clamp(dist / 160.0f, 0.25f, 1.4f);
		for (int a = 0; a < 3; ++a) flare.vel[a] = d[a] / time;
		flare.vel[2] += 0.5f * 120.0f * time; // gravity brings it down on the target
	}
	flare.age = 0.0f;
	flare.resting = false;
	_flares.push_back(flare);
	if (_flares.size() > FlareMax)
	{
		_flares.erase(_flares.begin());
	}
}

/**
 * Flares fly (gravity; heavy drag under the sea), bounce off what they hit, come to rest on
 * floors, burn with red fire (and a stream of bubbles under the sea) and die after a minute.
 */
void FirstPersonView::updateFlares(float dt)
{
	typedef FirstPersonParticles P;
	const float gravity = _look.underwater ? 40.0f : 120.0f;
	const int sx = _save->getMapSizeX(), sy = _save->getMapSizeY();
	for (auto &flare : _flares)
	{
		flare.age += dt;
		// flight, in small steps so it doesn't pass through walls
		for (float left = dt; left > 0.0f && !flare.resting; left -= 0.02f)
		{
			const float step = std::min(0.02f, left);
			flare.vel[2] -= gravity * step;
			if (_look.underwater)
			{
				for (float &v : flare.vel) v *= std::max(0.0f, 1.0f - 2.5f * step);
			}
			float to[3] = { flare.pos[0] + flare.vel[0] * step, flare.pos[1] + flare.vel[1] * step, flare.pos[2] + flare.vel[2] * step };
			float d[3] = { to[0] - flare.pos[0], to[1] - flare.pos[1], to[2] - flare.pos[2] };
			const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			const bool outside = to[0] < 0 || to[1] < 0 || to[0] >= sx * 16.0f || to[1] >= sy * 16.0f;
			if (length > 1e-4f && !outside)
			{
				for (float &v : d) v /= length;
				RayContext ctx;
				ctx.tiles = _tileViews.data();
				ctx.swings = _swings.data();
				ctx.size[0] = sx; ctx.size[1] = sy; ctx.size[2] = _save->getMapSizeZ();
				ctx.maxDistance = length;
				Hit hit;
				float tEnd;
				int endAxis;
				if (castRay(ctx, flare.pos, d, hit, tEnd, endAxis) == RAY_HIT && hit.t < length)
				{
					const int axis = hit.axis >= 0 ? hit.axis : 2;
					for (int a = 0; a < 3; ++a) flare.pos[a] += d[a] * std::max(0.0f, hit.t - 0.5f);
					const bool floor = axis == 2 && flare.vel[2] < 0.0f;
					flare.vel[axis] *= -0.3f; // bounce off it
					for (int a = 0; a < 3; ++a) if (a != axis) flare.vel[a] *= 0.5f;
					const float speed = std::sqrt(flare.vel[0] * flare.vel[0] + flare.vel[1] * flare.vel[1] + flare.vel[2] * flare.vel[2]);
					if (floor && speed < 30.0f)
					{
						flare.resting = true;
					}
					continue;
				}
			}
			std::copy(to, to + 3, flare.pos);
			if (outside && flare.pos[2] <= GroundTop + 0.5f)
			{
				flare.pos[2] = GroundTop + 0.5f; // the ground beyond the map edge
				flare.resting = true;
			}
			if (flare.pos[2] < 0.5f)
			{
				flare.pos[2] = 0.5f;
				flare.resting = true;
			}
		}
		// fire, smoke and (under the sea) bubbles, dying down at the end
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		if (Options::fpsParticles == 0 || fade <= 0.0f)
		{
			continue;
		}
		const int fire = (int)(dt * 45.0f * fade + _particles.random());
		for (int i = 0; i < fire; ++i)
		{
			P::Particle p;
			std::copy(flare.pos, flare.pos + 3, p.pos);
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(8, 20);
			p.age = 0; p.life = _particles.random(0.3f, 0.7f); p.gravity = 0; p.drag = 0.5f;
			p.color = _look.flareFire[(int)_particles.random(0, 2.99f)]; p.size = 2; p.kind = P::GLOW; p.group = P::EFFECT;
			_particles.add(p);
		}
		if (_particles.random() < dt * 6.0f * fade)
		{
			const Uint8 smoke[2] = { shadeColor(_look.flareFire[0], 6), _look.smoke };
			_particles.burst(flare.pos, 1, P::LIT, P::EFFECT, smoke, 2, 2, 6, 0.9f, -10, 0.6f, 1.2f, 2.0f, 3);
		}
		if (_look.underwater)
		{
			const int bubbles = (int)(dt * 35.0f * fade + _particles.random());
			for (int i = 0; i < bubbles; ++i)
			{
				P::Particle b;
				std::copy(flare.pos, flare.pos + 3, b.pos);
				b.vel[0] = _particles.random(-3, 3); b.vel[1] = _particles.random(-3, 3); b.vel[2] = _particles.random(12, 24);
				b.age = 0; b.life = _particles.random(3, 5); b.gravity = 0; b.drag = 0;
				b.color = _look.bubble; b.size = (Uint8)(1 + (int)(_particles.random() * 3.0f)); b.kind = P::BUBBLE; b.group = P::EFFECT;
				_particles.add(b);
			}
		}
	}
	_flares.erase(std::remove_if(_flares.begin(), _flares.end(), [](const Flare &f) { return f.age > FlareLife; }), _flares.end());
}

/**
 * How much a point is lit by the flares (0..1): close, facing them and (with shadows) not
 * behind anything; a slow flicker, fading out at the end.
 */
float FirstPersonView::flareLight(const float p[3], const float n[3], bool shadows) const
{
	const float time = SDL_GetTicks() / 1000.0f;
	const float darkness = _look.underwater ? 0.6f : 0.3f + 0.7f * _look.night; // a flare matters most in the dark
	// the strongest flare that isn't hidden lights the point (they don't add up); rank them
	// first so at most a couple of shadow rays are cast
	float powers[FlareMax];
	float targets[FlareMax][3];
	int order[FlareMax], count = 0;
	for (size_t i = 0; i < _flares.size() && count < (int)FlareMax; ++i)
	{
		const Flare &flare = _flares[i];
		const float at[3] = { flare.pos[0], flare.pos[1], flare.pos[2] + 2.0f };
		const float dx = at[0] - p[0], dy = at[1] - p[1], dz = at[2] - p[2];
		const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (dist > FlareReach || dist < 0.01f) continue;
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		const float flicker = 0.85f + 0.075f * (std::sin(time * 13.0f + i * 1.7f) + std::sin(time * 7.1f + i * 3.1f));
		float power = (1.0f - dist / FlareReach) * (1.0f - dist / FlareReach) * fade * flicker * darkness;
		if (n)
		{
			const float facing = (n[0] * dx + n[1] * dy + n[2] * dz) / dist;
			if (facing <= 0.0f) continue;
			power *= 0.4f + 0.6f * facing;
		}
		if (power < 0.02f) continue;
		powers[count] = power;
		std::copy(at, at + 3, targets[count]);
		order[count] = count;
		++count;
	}
	std::sort(order, order + count, [&powers](int a, int b) { return powers[a] > powers[b]; });
	for (int k = 0; k < count && k < 2; ++k)
	{
		// a faint glow casts no shadow worth a ray
		if (!shadows || powers[order[k]] < 0.08f || !shadowed(p, targets[order[k]]))
		{
			return powers[order[k]];
		}
	}
	return 0.0f;
}

/**
 * The flares themselves: a small red-hot glow where each one lies.
 */
void FirstPersonView::drawFlares()
{
	const int w = getWidth(), h = getHeight();
	const double focal = (w / 2.0) / _fovTan;
	for (const auto &flare : _flares)
	{
		const double world[3] = { flare.pos[0], flare.pos[1], flare.pos[2] + 0.5 };
		double sx, sy, zc;
		if (!project(world, sx, sy, zc) || zc < 3.0) continue;
		const float fade = std::clamp((FlareLife - flare.age) / 8.0f, 0.0f, 1.0f);
		if (fade <= 0.0f) continue;
		const int r = std::clamp((int)std::lround(2.0 * focal / zc * 0.5), 1, 6);
		for (int y = (int)sy - r; y <= (int)sy + r; ++y)
		{
			for (int x = (int)sx - r; x <= (int)sx + r; ++x)
			{
				const double d = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
				if (d > r || x < 0 || y < 0 || x >= w || y >= h || zc >= _depth[y * w + x] + 1.0f) continue;
				*getRaw(x, y) = d < r * 0.5 ? _look.flareFire[2] : _look.flareFire[0];
				_glowMask[y * w + x] = 1;
			}
		}
	}
}

/**
 * Leaves a mark where a shot hit: a bullet hole, or a bigger scorch from energy weapons.
 */
void FirstPersonView::addDecal(const float pos[3], int size, int depth)
{
	_decals.push_back({ { pos[0], pos[1], pos[2] }, (Uint8)size, (Uint8)depth });
	if (_decals.size() > 400)
	{
		_decals.erase(_decals.begin());
	}
}

/**
 * Bullet holes and scorch marks on whatever surface is there, and blackened ground around
 * explosions. Drawn before the units, which stand in front of them.
 */
void FirstPersonView::drawDecals()
{
	const int w = getWidth(), h = getHeight();
	const double focal = (w / 2.0) / _fovTan;
	for (const auto &scorch : _scorches)
	{
		const double centre[3] = { scorch.pos[0], scorch.pos[1], scorch.pos[2] };
		drawGroundShade(centre, scorch.radius, 4, 1.4f);
	}
	for (const auto &decal : _decals)
	{
		const double world[3] = { decal.pos[0], decal.pos[1], decal.pos[2] };
		double sx, sy, zc;
		if (!project(world, sx, sy, zc) || zc < 4.0) continue;
		const int r = std::clamp((int)std::lround(decal.size * focal / zc * 0.3), 0, 6);
		for (int y = (int)sy - r; y <= (int)sy + r; ++y)
		{
			for (int x = (int)sx - r; x <= (int)sx + r; ++x)
			{
				if (x < 0 || y < 0 || x >= w || y >= h) continue;
				const double d = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
				if (d > r + 0.5 || std::abs(_depth[y * w + x] - zc) > 2.5) continue; // on the surface it hit
				if (r > 1 && d > r * 0.6 && dither(x, y) > 0.5f) continue; // ragged edge
				Uint8 &pixel = *getRaw(x, y);
				if (pixel) pixel = (Uint8)((pixel & 0xF0) | std::min(15, (pixel & 15) + decal.depth));
			}
		}
	}
}

/**
 * Something on a tile was destroyed (blown up, burnt, shot away): chunks of it fly and a
 * cloud of dust rises.
 */
void FirstPersonView::debris(const Position &tile, int part, const MapData *before)
{
	typedef FirstPersonParticles P;
	const VoxelModel *model = getModel(before);
	Uint8 colors[2] = { _look.dust, _look.smoke };
	for (int z = model->min[2], found = 0; z <= model->max[2] && found < 2; z += 3)
	{
		const Uint8 color = model->color[z][8][8] ? model->color[z][8][8] : model->color[z][2][8];
		if (color) colors[found++] = color;
	}
	// from where the piece stood: walls on the tile's west or north edge
	const float centre[3] = { tile.x * 16.0f + (part == O_WESTWALL ? 1.0f : 8.0f), tile.y * 16.0f + (part == O_NORTHWALL ? 1.0f : 8.0f),
		tile.z * 24.0f + (model->min[2] + model->max[2]) * 0.5f };
	_particles.burst(centre, 24, P::LIT, P::EFFECT, colors, 2, 20, 80, 0.6f, 200, 0.4f, 0.8f, 1.8f, 3);
	const Uint8 dust[2] = { _look.dust, _look.smoke };
	_particles.burst(centre, 10, P::LIT, P::EFFECT, dust, 2, 3, 14, 0.5f, -8, 1.2f, 1.5f, 3.0f, 6);
}

/**
 * What a shot leaves behind along the bit of its path it flew this frame: nothing for bullets,
 * embers for incendiary rounds, smoke for rockets and other explosives, green glow for plasma,
 * sparkles for stun, droplets for acid. Under the sea every shot (and thrown thing) also leaves
 * bubbles that hang in the water for half a minute.
 */
void FirstPersonView::trailSegment(const float from[3], const float to[3])
{
	typedef FirstPersonParticles P;
	const float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (length < 0.5f || length > 400.0f)
	{
		return;
	}
	auto along = [&](float t, float out[3]) { for (int a = 0; a < 3; ++a) out[a] = from[a] + d[a] * t; };
	const int steps = std::max(1, (int)(length / 3.0f));
	for (int i = 0; i < steps; ++i)
	{
		float pos[3];
		along((i + _particles.random()) / steps, pos);
		P::Particle p;
		std::copy(pos, pos + 3, p.pos);
		p.age = 0; p.gravity = 0; p.drag = 0; p.group = P::EFFECT;
		bool add = true;
		switch (_lastProjectileThrown ? TRAIL_NONE : _trail)
		{
		case TRAIL_FIRE:
			p.vel[0] = _particles.random(-3, 3); p.vel[1] = _particles.random(-3, 3); p.vel[2] = _particles.random(2, 8);
			p.life = _particles.random(0.3f, 0.7f); p.color = _look.fire[(int)_particles.random(0, 2.99f)]; p.size = 2; p.kind = P::GLOW;
			break;
		case TRAIL_SMOKE:
			p.vel[0] = _particles.random(-2, 2); p.vel[1] = _particles.random(-2, 2); p.vel[2] = _particles.random(1, 4);
			p.life = _particles.random(1.5f, 3.0f); p.color = _particles.random() < 0.5f ? _look.smoke : shadeColor(_look.smoke, 2); p.size = 4; p.kind = P::LIT;
			p.drag = 0.8f;
			break;
		case TRAIL_PLASMA:
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(-4, 4);
			p.life = _particles.random(0.2f, 0.5f); p.color = _trailColor; p.size = 2; p.kind = P::GLOW;
			break;
		case TRAIL_STUN:
			p.vel[0] = _particles.random(-6, 6); p.vel[1] = _particles.random(-6, 6); p.vel[2] = _particles.random(-6, 6);
			p.life = _particles.random(0.15f, 0.35f); p.color = _trailColor; p.size = 1; p.kind = P::BLINK;
			break;
		case TRAIL_ACID:
			p.vel[0] = _particles.random(-2, 2); p.vel[1] = _particles.random(-2, 2); p.vel[2] = 0;
			p.life = _particles.random(0.4f, 0.8f); p.gravity = 90; p.color = _trailColor; p.size = 2; p.kind = P::LIT;
			add = _particles.random() < 0.5f;
			break;
		default:
			add = false; // bullets, darts, harpoons, beams (drawn as lines): no trail
			break;
		}
		if (add) _particles.add(p);
		if (_look.underwater && (i % 2 == 0))
		{
			P::Particle b;
			along((i + _particles.random()) / steps, b.pos);
			for (float &v : b.pos) v += _particles.random(-1.0f, 1.0f);
			b.vel[0] = _particles.random(-0.6f, 0.6f); b.vel[1] = _particles.random(-0.6f, 0.6f); b.vel[2] = _particles.random(0.8f, 2.5f);
			b.age = 0; b.life = _particles.random(22.0f, 34.0f); b.gravity = 0; b.drag = 0.05f;
			b.color = _look.bubble; b.size = (Uint8)(1 + (int)(_particles.random() * 2.0f)); b.kind = P::BUBBLE; b.group = P::EFFECT;
			_particles.add(b);
		}
	}
}

/**
 * A line in the world, depth-tested, a pixel or two thick, faded by dithering; it glows.
 */
void FirstPersonView::drawLine3D(const float a[3], const float b[3], Uint8 color, int thickness, float opacity)
{
	const int w = getWidth(), h = getHeight();
	// clip against a near plane so a beam from right beside the camera still draws
	double pa[3] = { a[0], a[1], a[2] }, pb[3] = { b[0], b[1], b[2] };
	auto depthOf = [&](const double p[3]) { return (p[0] - _eye[0]) * _fwd[0] + (p[1] - _eye[1]) * _fwd[1] + (p[2] - _eye[2]) * ZScale * _fwd[2]; };
	const double nearZ = 3.0, za = depthOf(pa), zb = depthOf(pb);
	if (za < nearZ && zb < nearZ) return;
	if (za < nearZ || zb < nearZ)
	{
		const double t = (nearZ - za) / (zb - za);
		double *clip = za < nearZ ? pa : pb;
		for (int i = 0; i < 3; ++i) clip[i] = pa[i] + (pb[i] - pa[i]) * t;
	}
	double ax, ay, az, bx, by, bz;
	if (!project(pa, ax, ay, az) || !project(pb, bx, by, bz)) return;
	const int steps = std::max(1, (int)std::max(std::abs(bx - ax), std::abs(by - ay)));
	if (steps > 4000) return;
	for (int i = 0; i <= steps; ++i)
	{
		const double t = (double)i / steps;
		const int x = (int)(ax + (bx - ax) * t), y = (int)(ay + (by - ay) * t);
		const float z = (float)(1.0 / (1.0 / az + (1.0 / bz - 1.0 / az) * t)); // depth is linear in 1/z on screen
		for (int oy = 0; oy < thickness; ++oy)
		{
			for (int ox = 0; ox < thickness; ++ox)
			{
				const int px = x + ox - thickness / 2, py = y + oy - thickness / 2;
				if (px < 0 || py < 0 || px >= w || py >= h || z >= _depth[py * w + px] + 1.0f) continue;
				if (opacity < 1.0f && dither(px, py) >= opacity) continue;
				*getRaw(px, py) = color;
				_glowMask[py * w + px] = 1;
			}
		}
	}
}

void FirstPersonView::showHurt(const double *from, float amount, bool splatter)
{
	const Uint32 now = SDL_GetTicks();
	_hurtFromKnown = false;
	if (from)
	{
		const double dx = from[0] - _eye[0], dy = from[1] - _eye[1];
		if (dx * dx + dy * dy > 4.0)
		{
			_hurtFrom[0] = dx;
			_hurtFrom[1] = dy;
			_hurtFromKnown = true;
			_hurtStart = now;
		}
	}
	if (splatter)
	{
		// blobs of blood near the edges of the view, more the harder the hit
		_splats.clear();
		const int count = 6 + (int)(amount * 10.0f);
		for (int i = 0; i < count; ++i)
		{
			auto next = [this]() { _splatSeed = _splatSeed * 1664525u + 1013904223u; return (_splatSeed >> 8) / 16777216.0f; };
			const float angle = next() * 2.0f * (float)Pi, edge = 0.55f + 0.4f * next();
			_splats.push_back({ 0.5f + 0.5f * edge * std::cos(angle), 0.5f + 0.5f * edge * std::sin(angle), 0.015f + 0.04f * next() * (0.5f + amount) });
		}
		_splatStart = now;
		_splatAmount = std::clamp(amount, 0.4f, 1.0f);
	}
}

/**
 * How a shot looks in flight, from its ammo's damage type (trail kind and colour).
 */
void FirstPersonView::trailFor(const BattleItem *ammo, Trail &trail, Uint8 &color) const
{
	const RuleDamageType *damage = ammo ? ammo->getRules()->getDamageType() : nullptr;
	const SDL_Color *pal = const_cast<FirstPersonView*>(this)->getPalette();
	auto nearest = [pal](int r, int g, int b) {
		int best = 1, bestDist = INT_MAX;
		for (int i = 1; i < 256; ++i)
		{
			const int dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
			if (dr * dr + dg * dg + db * db < bestDist) { bestDist = dr * dr + dg * dg + db * db; best = i; }
		}
		return (Uint8)best;
	};
	switch (damage ? damage->ResistType : DT_NONE)
	{
	case DT_IN: trail = TRAIL_FIRE; color = _look.fire[1]; break;
	case DT_HE: trail = TRAIL_SMOKE; color = _look.smoke; break;
	case DT_LASER: trail = TRAIL_BEAM; color = _look.underwater ? nearest(90, 220, 255) : nearest(255, 40, 30); break;
	case DT_PLASMA: trail = TRAIL_PLASMA; color = _look.underwater ? nearest(180, 240, 255) : nearest(90, 255, 90); break;
	case DT_STUN: trail = TRAIL_STUN; color = nearest(160, 190, 255); break;
	case DT_ACID: trail = TRAIL_ACID; color = nearest(190, 255, 60); break;
	default: trail = TRAIL_NONE; color = _look.spark; break;
	}
}

/**
 * Where a shot ended (not in a unit: that bleeds or sparks, see watchUnits): sparks, debris
 * and a puff of dust, and a bullet hole, or a scorch from energy weapons and incendiaries.
 */
void FirstPersonView::shotLanded(const float head[3], Trail trail)
{
	typedef FirstPersonParticles P;
	const Position headTile((int)head[0] / 16, (int)head[1] / 16, (int)head[2] / 24);
	const Tile *headAt = _save->getTile(headTile);
	if (headAt && (headAt->getUnit() || (headTile.z > 0 && _save->getTile(headTile - Position(0, 0, 1))->getUnit())))
	{
		return;
	}
	const Uint8 debris = colorAt(head);
	const Uint8 sparks[3] = { _look.spark, _look.fire[1], _look.fire[0] };
	const Uint8 chunks[2] = { debris, shadeColor(debris, 2) };
	const Uint8 dust[2] = { _look.dust, _look.smoke };
	_particles.burst(head, 14, P::GLOW, P::EFFECT, sparks, 3, 30, 90, 0.4f, 160, 1.0f, 0.2f, 0.5f, 2);
	_particles.burst(head, 8, P::LIT, P::EFFECT, chunks, 2, 20, 60, 0.6f, 220, 0.5f, 0.6f, 1.1f, 2);
	_particles.burst(head, 6, P::LIT, P::EFFECT, dust, 2, 3, 10, 0.5f, -6, 1.5f, 0.8f, 1.6f, 4);
	const bool burn = trail == TRAIL_BEAM || trail == TRAIL_PLASMA || trail == TRAIL_FIRE;
	addDecal(head, burn ? 3 : 1, burn ? 6 : 4);
}

/**
 * The head of a shot in flight: a small dot for bullets, a glowing blob for plasma, a flame
 * for rockets.
 */
void FirstPersonView::drawShotDot(const float head[3], Trail trail, Uint8 color)
{
	const double world[3] = { head[0], head[1], head[2] };
	double sx, sy, zc;
	if (!project(world, sx, sy, zc) || zc < 6.0)
	{
		return;
	}
	const int w = getWidth(), h = getHeight();
	const double focal = (w / 2.0) / _fovTan;
	const double size = trail == TRAIL_PLASMA ? 5.0 : trail == TRAIL_SMOKE ? 3.0 : 1.2;
	const int r = std::clamp((int)std::lround(size * focal / zc * 0.5), 1, 12);
	const Uint8 core = trail == TRAIL_NONE ? _look.spark : trail == TRAIL_SMOKE ? _look.fire[0] : color;
	for (int y = (int)sy - r; y <= (int)sy + r; ++y)
	{
		for (int x = (int)sx - r; x <= (int)sx + r; ++x)
		{
			const double d = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
			if (d > r || x < 0 || y < 0 || x >= w || y >= h || zc >= _depth[y * w + x]) continue;
			*getRaw(x, y) = d < r * 0.5 ? (Uint8)(core & 0xF0) : core; // white-hot centre
			if (trail != TRAIL_NONE) _glowMask[y * w + x] = 1;
		}
	}
}

/**
 * Dual wield: the other gun's round, fired with ours: from the other gun's muzzle on a straight
 * line to where it lands, with its trail; it kicks and lights up the room too, and lands with
 * sparks, a hole or a beam like any of our shots.
 */
void FirstPersonView::spawnPartnerEffects()
{
	const Projectile *partner = _map ? _map->getPartnerProjectile() : nullptr;
	if (partner != _lastPartner)
	{
		if (_lastPartner)
		{
			shotLanded(_partnerHead, _partnerTrail);
			if (_partnerTrail == TRAIL_BEAM)
			{
				_beams.push_back({ { _partnerOrigin[0], _partnerOrigin[1], _partnerOrigin[2] }, { _partnerHead[0], _partnerHead[1], _partnerHead[2] }, SDL_GetTicks(), _partnerColor });
			}
		}
		if (partner)
		{
			const Position from = partner->getPosition(-100000), end = partner->getPosition(100000);
			float origin[3] = { (float)from.x, (float)from.y, (float)from.z };
			std::copy(origin, origin + 3, _partnerFrom);
			_partnerLength = std::max(1.0f, (float)std::sqrt(Position::distanceSq(from, end)));
			std::fill(_partnerOffset, _partnerOffset + 3, 0.0f);
			if (_offMuzzleValid)
			{
				double dir[3];
				getRayDirection((int)_offMuzzleScreen[0], (int)_offMuzzleScreen[1], dir);
				for (int a = 0; a < 3; ++a)
				{
					origin[a] = (float)(_eye[a] + dir[a] * 12.0);
					_partnerOffset[a] = origin[a] - _partnerFrom[a];
				}
			}
			std::copy(origin, origin + 3, _partnerOrigin);
			std::copy(origin, origin + 3, _partnerHead);
			trailFor(partner->getAmmo(), _partnerTrail, _partnerColor);
			_partnerShotTick = SDL_GetTicks();
			std::copy(origin, origin + 3, _muzzle); // the flash lights up the room
			_muzzleTick = _partnerShotTick;
			const float power = partner->getAmmo() ? (float)partner->getAmmo()->getRules()->getPower() : 30.0f;
			_kick = std::min(_kick + std::clamp(power / 30.0f, 0.5f, 2.5f), 6.0f);
		}
		_lastPartner = partner;
	}
	if (partner)
	{
		const float previous[3] = { _partnerHead[0], _partnerHead[1], _partnerHead[2] };
		const Position head = partner->getPosition(0);
		const float at[3] = { (float)head.x, (float)head.y, (float)head.z };
		float travelled = 0.0f;
		for (int a = 0; a < 3; ++a) travelled += (at[a] - _partnerFrom[a]) * (at[a] - _partnerFrom[a]);
		const float left = std::max(0.0f, 1.0f - std::sqrt(travelled) / _partnerLength);
		for (int a = 0; a < 3; ++a) _partnerHead[a] = at[a] + _partnerOffset[a] * left;
		std::swap(_trail, _partnerTrail);
		std::swap(_trailColor, _partnerColor);
		trailSegment(previous, _partnerHead);
		std::swap(_trail, _partnerTrail);
		std::swap(_trailColor, _partnerColor);
	}
}

/**
 * The soldier we look through is on fire: a sheet of flame rises from the bottom of the view,
 * tall in the corners and low in the middle so you can still see ahead. Heat rises cell by cell,
 * drifting sideways and cooling at random (the old fire effect of Doom on the PlayStation), in
 * blocks of the 320x200 view's pixels; dark red at the tips through orange and yellow to white
 * at the root. Embers drift up out of it, and its light flickers up from below. Over the world
 * (the weapon in hand still in front). A flare-up (catching fire) makes it leap; put out, the
 * last of the heat rises and dies away.
 */
void FirstPersonView::drawBurning()
{
	const Uint32 now = SDL_GetTicks();
	const float dt = _flameTick ? std::min<Uint32>(100, now - _flameTick) / 1000.0f : 0.0f;
	_flameTick = now;
	_flareUp = std::max(0.0f, _flareUp - dt * 1.2f);
	if (_onFire <= 0.0f)
	{
		_flareUp = 0.0f;
		if (!_fireLit && _embers.empty()) return;
	}
	const int w = getWidth(), h = getHeight();
	const int block = std::max(1, (int)std::lround(h / 200.0));
	const int fw = (w + block - 1) / block, fh = (int)(h * 0.7) / block;
	if (fw != _fireW || fh != _fireH)
	{
		_fireW = fw;
		_fireH = fh;
		_fireHeat.assign((size_t)fw * fh, 0);
	}
	auto rnd = [this]() { _fireSeed = _fireSeed * 1664525u + 1013904223u; return _fireSeed >> 8; };
	const int MaxHeat = 48;
	const float px = h / 200.0f; // sizes and speeds as on a 320x200 view
	const float fierce = std::min(1.0f, _onFire * (1.0f + 1.2f * _flareUp));
	const float flicker = 0.8f + 0.2f * std::sin(now * 0.031f) * std::sin(now * 0.017f + 1.0f);
	// the light of the flames, from below
	if (_onFire > 0.0f)
	{
		const int rows = (int)(h * 0.3f);
		for (int y = h - rows; y < h; ++y)
		{
			const float glow = std::pow((y - (h - rows)) / (float)rows, 1.6f) * 0.7f * fierce * flicker;
			Uint8 *row = getRaw(0, y);
			for (int x = 0; x < w; ++x)
			{
				if (dither(x, y) < glow) row[x] = _look.orangeTint[row[x]];
			}
		}
	}
	// the flames step 50 times a second: the bottom row is fed (hotter towards the corners),
	// every cell passes its heat up a row and a little downwind (the wind turns now and then),
	// a little cooler; the middle cools faster, so the flames stay low there (and a flare-up
	// cools them less)
	const int wind = std::sin(now * 0.0005f) > 0.0f ? 1 : -1;
	_fireStep = std::min(_fireStep + dt, 0.1f);
	while (_fireStep >= 0.02f)
	{
		_fireStep -= 0.02f;
		Uint8 *heat = _fireHeat.data();
		Uint8 *source = heat + (size_t)(fh - 1) * fw;
		const float t = now / 1000.0f;
		for (int x = 0; x < fw; ++x)
		{
			// tongues: hot roots wandering along the bottom
			const float cell = x * block / px;
			const float roots = 0.5f + 0.5f * std::sin(cell * 0.21f + t * 1.7f) * std::sin(cell * 0.083f - t * 1.1f + 1.3f);
			const float edge = std::abs(x - fw * 0.5f) / (fw * 0.5f);
			source[x] = _onFire > 0.0f ? (Uint8)(MaxHeat * fierce * (0.3f + 0.7f * roots) * (0.65f + 0.35f * edge) * (0.85f + 0.15f * (rnd() & 255) / 255.0f)) : 0;
		}
		// and the sides burn up, less the higher (a thin wall of fire framing the view)
		if (_onFire > 0.0f)
		{
			const int side = std::max(2, fw / 48);
			for (int y = 0; y < fh - 1; ++y)
			{
				const float up = (fh - 1 - y) / (float)(fh - 1);
				const float lick = 0.5f + 0.5f * std::sin(y * block / px * 0.12f + t * 2.3f);
				const Uint8 feed = (Uint8)std::max(0.0f, MaxHeat * fierce * (0.9f - 1.2f * up) * (0.6f + 0.4f * lick));
				for (int k = 0; k < side; ++k)
				{
					Uint8 &left = heat[y * fw + k], &right = heat[y * fw + fw - 1 - k];
					left = std::max(left, feed);
					right = std::max(right, feed);
				}
			}
		}
		bool lit = false;
		for (int y = 1; y < fh; ++y)
		{
			for (int x = 0; x < fw; ++x)
			{
				const Uint8 here = heat[y * fw + x];
				if (!here)
				{
					heat[(y - 1) * fw + x] = 0;
					continue;
				}
				lit = true;
				const Uint32 r = rnd();
				const int jitter = (r & 3) == 0 ? -1 : (r & 3) == 3 ? 1 : 0;
				const int to = std::clamp(x + jitter + (((r >> 2) & 7) == 0 ? wind : 0), 0, fw - 1);
				const float edge = std::abs(x - fw * 0.5f) / (fw * 0.5f);
				const float extra = (0.1f + (1.0f - edge) * (1.0f - edge) * 0.55f) * (1.0f - 0.6f * _flareUp);
				const int cool = (int)((r >> 4) & 1) + (((r >> 8) & 255) < extra * 255.0f ? 1 : 0);
				heat[(y - 1) * fw + to] = here > cool ? (Uint8)(here - cool) : 0;
			}
		}
		_fireLit = lit || _onFire > 0.0f;
	}
	// drawn in blocks: white at the root, yellow, orange, dark red, wispy at the tips
	const Uint8 ramp[5] = { shadeColor(_look.fire[3], 4), _look.fire[3], _look.fire[2], _look.fire[1], _look.fire[0] };
	for (int y = 0; y < fh; ++y)
	{
		const int sy = h - (fh - y) * block;
		for (int x = 0; x < fw; ++x)
		{
			const int here = _fireHeat[y * fw + x];
			if (!here) continue;
			const int sx = x * block;
			if (here < 7 && dither(sx, sy) >= here / 7.0f) continue;
			const float f = here / (float)MaxHeat * 4.0f;
			const int band = std::min(3, (int)f);
			const Uint8 color = dither(sx + 1, sy + 2) < f - band ? ramp[band + 1] : ramp[band];
			for (int by = std::max(0, sy); by < std::min(h, sy + block); ++by)
			{
				Uint8 *row = getRaw(0, by);
				for (int bx = sx; bx < std::min(w, sx + block); ++bx) row[bx] = color;
			}
		}
	}
	// embers drifting up out of it, twinkling
	_emberSpawn += dt * (_onFire > 0.0f ? 30.0f * fierce : 0.0f);
	while (_emberSpawn >= 1.0f)
	{
		_emberSpawn -= 1.0f;
		if (_embers.size() >= 200) continue;
		Ember e;
		const float u = _particles.random(-1.0f, 1.0f);
		e.x = w * 0.5f + (u < 0.0f ? -1.0f : 1.0f) * std::sqrt(std::abs(u)) * w * 0.5f;
		e.y = h - _particles.random(0.05f, 0.3f) * h;
		e.vx = _particles.random(-20.0f, 20.0f) * px;
		e.vy = -_particles.random(50.0f, 100.0f) * px;
		e.age = 0.0f;
		e.life = _particles.random(0.8f, 1.8f);
		e.phase = _particles.random(0.0f, 6.3f);
		_embers.push_back(e);
	}
	const int size = std::max(1, block);
	for (size_t i = 0; i < _embers.size();)
	{
		Ember &e = _embers[i];
		e.age += dt;
		if (e.age >= e.life)
		{
			e = _embers.back();
			_embers.pop_back();
			continue;
		}
		e.x += (e.vx + std::sin(e.age * 7.0f + e.phase) * 25.0f * px) * dt;
		e.y += e.vy * dt;
		++i;
		if (std::sin(e.age * 30.0f + e.phase) < -0.6f) continue;
		const Uint8 color = _look.fire[e.age < e.life * 0.5f ? 0 : e.age < e.life * 0.8f ? 1 : 2];
		const int x0 = (int)e.x, y0 = (int)e.y;
		for (int y = std::max(0, y0); y < std::min(h, y0 + size); ++y)
		{
			for (int x = std::max(0, x0); x < std::min(w, x0 + size); ++x) *getRaw(x, y) = color;
		}
	}
}

/**
 * Under fire: a red arc on the edge of the view towards where the last hit came from (it turns
 * with you), and blood on the view after a heavy or close hit; both fade by dithering.
 */
void FirstPersonView::drawHurt()
{
	const Uint32 now = SDL_GetTicks();
	const int w = getWidth(), h = getHeight();
	const Uint32 ArcTime = 1600, SplatTime = 1800;
	if (_hurtFromKnown && now - _hurtStart < ArcTime)
	{
		const float fade = 1.0f - (now - _hurtStart) / (float)ArcTime;
		// the direction relative to where we look: ahead is the top of the view, behind the bottom
		const double a = std::atan2(_hurtFrom[1], _hurtFrom[0]) - _yaw;
		const float cx = w * 0.5f, cy = h * 0.5f, rx = w * 0.44f, ry = h * 0.42f, thick = std::max(3.0f, w / 110.0f);
		for (int step = -20; step <= 20; ++step)
		{
			const double b = a + step * (Pi / 180.0) * 0.9;
			const float ex = cx + (float)std::sin(b) * rx, ey = cy - (float)std::cos(b) * ry;
			const float taper = 1.0f - std::abs(step) / 21.0f;
			const int r = (int)std::ceil(thick * (0.4f + 0.6f * taper));
			for (int y = (int)ey - r; y <= (int)ey + r; ++y)
			{
				for (int x = (int)ex - r; x <= (int)ex + r; ++x)
				{
					if (x < 0 || y < 0 || x >= w || y >= h || (x - ex) * (x - ex) + (y - ey) * (y - ey) > r * r) continue;
					if (dither(x, y) < fade * (0.35f + 0.65f * taper)) *getRaw(x, y) = _look.blood[1];
				}
			}
		}
	}
	if (!_splats.empty() && now - _splatStart < SplatTime)
	{
		const float fade = _splatAmount * (1.0f - (now - _splatStart) / (float)SplatTime);
		for (const Splat &splat : _splats)
		{
			const float sx = splat.x * w, sy = splat.y * h, r = splat.r * w;
			for (int y = std::max(0, (int)(sy - r)); y < std::min(h, (int)(sy + r) + 1); ++y)
			{
				for (int x = std::max(0, (int)(sx - r)); x < std::min(w, (int)(sx + r) + 1); ++x)
				{
					const float d = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy)) / r;
					if (d > 1.0f || dither(x + 1, y + 2) >= fade * (1.2f - d)) continue;
					*getRaw(x, y) = d < 0.6f ? _look.blood[0] : _look.blood[1];
				}
			}
		}
	}
}

void FirstPersonView::shotHead(const Projectile *projectile, float out[3]) const
{
	const Position head = projectile->getPosition(0);
	const float at[3] = { (float)head.x, (float)head.y, (float)head.z };
	float travelled = 0.0f;
	for (int a = 0; a < 3; ++a) travelled += (at[a] - _shotFrom[a]) * (at[a] - _shotFrom[a]);
	// the game's path is a straight line, so this draws a straight line from the muzzle to the hit
	const float left = std::max(0.0f, 1.0f - std::sqrt(travelled) / _shotLength);
	for (int a = 0; a < 3; ++a) out[a] = at[a] + _shotOffset[a] * left;
}

/**
 * Laser (and gauss) beams: a line from the muzzle to wherever the shot is (or ended), a bright
 * core in a coloured sheath, fading out over a third of a second after the hit.
 */
void FirstPersonView::drawBeams()
{
	const Uint32 now = SDL_GetTicks();
	_beams.erase(std::remove_if(_beams.begin(), _beams.end(), [now](const Beam &b) { return now - b.start > 350; }), _beams.end());
	for (const auto &beam : _beams)
	{
		const float fade = 1.0f - (now - beam.start) / 350.0f;
		drawLine3D(beam.from, beam.to, beam.color, 2, fade);
		drawLine3D(beam.from, beam.to, shadeColor(beam.color, 0) & 0xF0, 1, fade); // the white-hot core: the group's brightest shade
	}
	Projectile *projectile = _map ? _map->getProjectile() : nullptr;
	if (projectile && projectile == _lastProjectile && !_lastProjectileThrown && _trail == TRAIL_BEAM)
	{
		drawLine3D(_trailOrigin, _lastHead, _trailColor, 2, 1.0f);
		drawLine3D(_trailOrigin, _lastHead, (Uint8)(_trailColor & 0xF0), 1, 1.0f);
	}
	if (_map && _map->getPartnerProjectile() && _partnerTrail == TRAIL_BEAM)
	{
		drawLine3D(_partnerOrigin, _partnerHead, _partnerColor, 2, 1.0f);
		drawLine3D(_partnerOrigin, _partnerHead, (Uint8)(_partnerColor & 0xF0), 1, 1.0f);
	}
}

/**
 * Keeps the air alive around the camera: dust motes by day, fireflies outside at night,
 * bubbles under the sea, and the terrain's weather (rain, snow, blowing sand) where the
 * sky is open above.
 */
void FirstPersonView::spawnAmbient(float dt)
{
	typedef FirstPersonParticles P;
	if (!_weatherChosen)
	{
		_weatherChosen = true;
		_weather.clear();
		for (const MapDataSet *set : *_save->getMapDataSets())
		{
			const std::string weather = _game->getMod()->getFirstPerson().getWeather(set->getName());
			if (!weather.empty())
			{
				_weather = weather;
				break;
			}
		}
	}
	const float e[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
	// what the camera left behind is gone, and comes back around it
	_particles.cull(P::AMBIENT, e[0], e[1], 130);
	_particles.cull(P::WEATHER, e[0], e[1], 150);

	// motes, fireflies or bubbles (bubbles all through the water)
	const int ambientTarget = _look.underwater ? 600 : (_look.night > 0.6f ? 30 : 90);
	for (int n = _particles.count(P::AMBIENT); n < ambientTarget; ++n)
	{
		P::Particle p;
		const float spread = 90.0f;
		p.pos[0] = e[0] + _particles.random(-spread, spread);
		p.pos[1] = e[1] + _particles.random(-spread, spread);
		if (_look.underwater && _particles.random() < 0.6f)
		{
			// most of them where the camera looks
			const float yaw = (float)_yaw + _particles.random(-1.2f, 1.2f), dist = _particles.random(8.0f, spread);
			p.pos[0] = e[0] + std::cos(yaw) * dist;
			p.pos[1] = e[1] + std::sin(yaw) * dist;
		}
		p.pos[2] = _look.underwater ? _particles.random(GroundTop, e[2] + 12.0f) : e[2] + _particles.random(-18, 30);
		p.age = 0;
		p.group = P::AMBIENT;
		p.size = 1;
		if (_look.underwater)
		{
			if (!inWater(p.pos[0], p.pos[1], p.pos[2])) continue; // not inside the sub, a building or a rock
			p.vel[0] = _particles.random(-2, 2); p.vel[1] = _particles.random(-2, 2); p.vel[2] = _particles.random(6, 16);
			p.life = _particles.random(2.5f, 5.0f); p.gravity = 0; p.drag = 0; p.color = _look.bubble; p.kind = P::BUBBLE;
			p.size = (Uint8)(2 + (int)(_particles.random() * 3.0f)); // 2..4
		}
		else if (_look.night > 0.6f)
		{
			if (!openSky(p.pos[0], p.pos[1], p.pos[2])) continue;
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(-2, 2);
			p.life = _particles.random(3, 8); p.gravity = 0; p.drag = 0; p.color = _look.firefly; p.kind = P::BLINK;
		}
		else
		{
			p.vel[0] = _particles.random(-1.5f, 1.5f); p.vel[1] = _particles.random(-1.5f, 1.5f); p.vel[2] = _particles.random(-0.8f, 0.8f);
			p.life = _particles.random(4, 9); p.gravity = 0; p.drag = 0; p.color = _look.dust; p.kind = P::LIT;
		}
		_particles.add(p);
	}

	if (_look.underwater)
	{
		// columns of bubbles from a few spots on the sea floor around the camera
		float cx = 0, cy = 0;
		for (const auto &vent : _vents) { cx += vent.first; cy += vent.second; }
		if (!_vents.empty()) { cx /= _vents.size(); cy /= _vents.size(); }
		if (_vents.empty() || (cx - e[0]) * (cx - e[0]) + (cy - e[1]) * (cy - e[1]) > 90.0f * 90.0f)
		{
			_vents.clear();
			for (int i = 0; i < 24 && _vents.size() < 7; ++i)
			{
				const float x = e[0] + _particles.random(-110, 110), y = e[1] + _particles.random(-110, 110);
				if (inWater(x, y, GroundTop + 1.0f)) _vents.push_back({ x, y });
			}
		}
		for (const auto &vent : _vents)
		{
			if (_particles.random() > dt * 9.0f) continue;
			P::Particle p;
			p.pos[0] = vent.first + _particles.random(-1.5f, 1.5f);
			p.pos[1] = vent.second + _particles.random(-1.5f, 1.5f);
			p.pos[2] = GroundTop + 1.0f;
			p.vel[0] = _particles.random(-1.5f, 1.5f); p.vel[1] = _particles.random(-1.5f, 1.5f); p.vel[2] = _particles.random(14, 22);
			p.age = 0; p.life = _particles.random(4, 6); p.gravity = 0; p.drag = 0;
			p.color = _look.bubble; p.size = (Uint8)(2 + (int)(_particles.random() * 2.0f)); p.kind = P::BUBBLE; p.group = P::EFFECT;
			_particles.add(p);
		}
		// marine snow: specks hanging in the water, drifting with the current
		const int target = 1200;
		const int have = _particles.count(P::WEATHER);
		const int toSpawn = have < target / 2 ? target - have : std::min(target - have, (int)(target * dt * 0.3f) + 1);
		for (int n = 0; n < toSpawn; ++n)
		{
			P::Particle p;
			// most of them where the camera looks, and about eye high
			const float angle = _particles.random() < 0.6f ? (float)_yaw + _particles.random(-1.2f, 1.2f) : _particles.random(0, 2 * (float)Pi);
			const float radius = 100 * std::sqrt(_particles.random());
			p.pos[0] = e[0] + std::cos(angle) * radius;
			p.pos[1] = e[1] + std::sin(angle) * radius;
			p.pos[2] = _particles.random(GroundTop, e[2] + 25.0f);
			if (!inWater(p.pos[0], p.pos[1], p.pos[2])) continue;
			p.vel[0] = _particles.random(0.5f, 3.0f); p.vel[1] = _particles.random(-1.5f, 1.5f); p.vel[2] = _particles.random(-1.5f, -0.3f);
			p.age = 0; p.life = _particles.random(6, 11); p.gravity = 0; p.drag = 0;
			p.color = _look.marine; p.size = 1; p.kind = P::LIT; p.group = P::WEATHER;
			_particles.add(p);
		}
		return;
	}

	// weather
	if (_weather.empty())
	{
		return;
	}
	const bool rain = _weather == "rain", snow = _weather == "snow";
	const int target = rain ? 1500 : snow ? 1400 : 300;
	const int have = _particles.count(P::WEATHER);
	// spread over time so they don't land together, but fill up at once after a jump
	const int toSpawn = have < target / 2 ? target - have : std::min(target - have, (int)(target * dt * 1.5f) + 1);
	for (int n = 0; n < toSpawn; ++n)
	{
		P::Particle p;
		// evenly over a disc around the camera: most of what's seen falls close by
		const float angle = _particles.random(0, 2 * (float)Pi), radius = 130 * std::sqrt(_particles.random());
		p.pos[0] = e[0] + std::cos(angle) * radius;
		p.pos[1] = e[1] + std::sin(angle) * radius;
		p.age = 0;
		p.group = P::WEATHER;
		p.drag = 0;
		p.gravity = 0;
		if (rain || snow)
		{
			p.pos[2] = _particles.random(std::max((float)GroundTop, e[2] - 25), e[2] + 45);
			const float fall = rain ? _particles.random(260, 320) : _particles.random(18, 30);
			p.vel[0] = rain ? 30 : _particles.random(-6, 6); p.vel[1] = rain ? 10 : _particles.random(-6, 6); p.vel[2] = -fall;
			p.life = (p.pos[2] - GroundTop) / fall;
			// flakes in two tones, so they read against white clouds and snow as well as blue sky
			p.color = rain ? _look.rain : (_particles.random() < 0.5f ? _look.snow : shadeColor(_look.snow, 3));
			p.size = rain ? 1 : 5;
			p.kind = rain ? P::STREAK : P::LIT;
		}
		else
		{
			p.pos[2] = GroundTop + _particles.random(0, 40);
			p.vel[0] = _particles.random(40, 70); p.vel[1] = _particles.random(5, 20); p.vel[2] = _particles.random(-3, 3);
			p.life = _particles.random(2, 4);
			p.color = _look.sand;
			p.size = 1;
			p.kind = P::LIT;
		}
		if (!openSky(p.pos[0], p.pos[1], std::max(GroundTop, p.pos[2] - 100)))
		{
			continue; // under a roof
		}
		_particles.add(p);
	}
}

/**
 * Brings the machinery and lights to life: energy rising off UFO power sources with the odd
 * crackle of sparks, status lights blinking on navigation consoles, and (at night or under
 * the sea) motes drifting in the glow of lamps.
 */
void FirstPersonView::spawnDevices(float dt)
{
	typedef FirstPersonParticles P;
	for (const auto &device : _devices)
	{
		if (device.power)
		{
			// energy spiralling up around the core, just off its surface
			const int count = (int)(dt * 50.0f + _particles.random());
			const float radius = std::max(device.half[0], device.half[1]) * 0.9f + 1.5f;
			for (int i = 0; i < count; ++i)
			{
				P::Particle p;
				const float angle = _particles.random(0, 2 * (float)Pi);
				const float ca = std::cos(angle), sa = std::sin(angle);
				p.pos[0] = device.pos[0] + ca * radius;
				p.pos[1] = device.pos[1] + sa * radius;
				p.pos[2] = device.pos[2] + _particles.random(-device.half[2], device.half[2]);
				const float swirl = _particles.random(5, 9);
				p.vel[0] = -sa * swirl + ca * 1.5f; p.vel[1] = ca * swirl + sa * 1.5f; p.vel[2] = _particles.random(9, 18);
				p.age = 0; p.life = _particles.random(1.0f, 2.2f); p.gravity = 0; p.drag = 0.3f;
				p.color = _particles.random() < 0.5f ? _look.fire[0] : _look.fire[1];
				p.size = _particles.random() < 0.4f ? 3 : 2;
				p.kind = P::GLOW; p.group = P::EFFECT;
				_particles.add(p);
			}
			if (_particles.random() < dt * 0.7f)
			{
				const float at[3] = { device.pos[0], device.pos[1], device.pos[2] + device.half[2] * 0.5f };
				const Uint8 sparks[2] = { _look.spark, _look.fire[0] };
				_particles.burst(at, 10, P::GLOW, P::EFFECT, sparks, 2, 20, 50, 0.5f, 60, 0.5f, 0.2f, 0.6f, 1);
			}
		}
		else if (_particles.random() < dt * 4.0f)
		{
			// a status light somewhere on top of the console
			static const Uint8 tints[3][3] = { { 240, 60, 40 }, { 80, 230, 90 }, { 90, 160, 255 } };
			const Uint8 *tint = tints[(int)(_particles.random() * 2.999f)];
			const SDL_Color *pal = getPalette();
			int best = 1, bestDist = INT_MAX;
			for (int i = 1; i < 256; ++i)
			{
				const int r = pal[i].r - tint[0], g = pal[i].g - tint[1], b = pal[i].b - tint[2];
				if (r * r + g * g + b * b < bestDist) { bestDist = r * r + g * g + b * b; best = i; }
			}
			P::Particle p;
			p.pos[0] = device.pos[0] + _particles.random(-device.half[0], device.half[0]) * 0.8f;
			p.pos[1] = device.pos[1] + _particles.random(-device.half[1], device.half[1]) * 0.8f;
			p.pos[2] = device.pos[2] + device.half[2] + 0.5f;
			p.vel[0] = p.vel[1] = p.vel[2] = 0;
			p.age = 0; p.life = _particles.random(1.5f, 3.5f); p.gravity = 0; p.drag = 0;
			p.color = (Uint8)best; p.size = 2; p.kind = P::BLINK; p.group = P::EFFECT;
			_particles.add(p);
		}
	}
	// motes in the lamplight
	if (_look.night > 0.4f || _look.underwater)
	{
		for (const auto &light : _lights)
		{
			const float dx = light.pos[0] - (float)_eye[0], dy = light.pos[1] - (float)_eye[1];
			if (dx * dx + dy * dy > 200.0f * 200.0f || _particles.random() > dt * 2.5f * light.strength) continue;
			P::Particle p;
			p.pos[0] = light.pos[0] + _particles.random(-6, 6);
			p.pos[1] = light.pos[1] + _particles.random(-6, 6);
			p.pos[2] = light.pos[2] + _particles.random(-6, 4);
			p.vel[0] = _particles.random(-4, 4); p.vel[1] = _particles.random(-4, 4); p.vel[2] = _particles.random(-2, 3);
			p.age = 0; p.life = _particles.random(1.5f, 3.5f); p.gravity = 0; p.drag = 0;
			p.color = light.color; p.size = 1; p.kind = P::GLOW; p.group = P::EFFECT;
			_particles.add(p);
		}
	}
}

/**
 * Bloom: whatever shines (the lit parts of lamps and screens, sparks, embers, flashes) spills
 * a soft glow over its surroundings. The shining pixels are gathered at quarter resolution,
 * blurred, and the image is brightened (within each colour's own shades, dithered) where
 * that glow lands; it shows most at night and under the sea.
 */
void FirstPersonView::drawHalos()
{
	const int w = getWidth(), h = getHeight();
	const int cell = 4, gw = (w + cell - 1) / cell, gh = (h + cell - 1) / cell;
	_glowBuf.assign(gw * gh, 0.0f);
	_glowTmp.assign(gw * gh, 0.0f);
	bool any = false;
	for (int y = 0; y < h; ++y)
	{
		const Uint8 *mask = &_glowMask[y * w];
		float *row = &_glowBuf[(y / cell) * gw];
		for (int x = 0; x < w; ++x)
		{
			if (mask[x])
			{
				row[x / cell] += 1.0f / (cell * cell);
				any = true;
			}
		}
	}
	if (!any)
	{
		return;
	}
	// two separable box blurs, close to a gaussian
	const int radius = 3;
	for (int pass = 0; pass < 2; ++pass)
	{
		for (int y = 0; y < gh; ++y)
		{
			for (int x = 0; x < gw; ++x)
			{
				float sum = 0.0f;
				for (int i = -radius; i <= radius; ++i) sum += _glowBuf[y * gw + std::clamp(x + i, 0, gw - 1)];
				_glowTmp[y * gw + x] = sum / (2 * radius + 1);
			}
		}
		for (int y = 0; y < gh; ++y)
		{
			for (int x = 0; x < gw; ++x)
			{
				float sum = 0.0f;
				for (int i = -radius; i <= radius; ++i) sum += _glowTmp[std::clamp(y + i, 0, gh - 1) * gw + x];
				_glowBuf[y * gw + x] = sum / (2 * radius + 1);
			}
		}
	}
	// steady (a flickering bloom makes the dithered glow shimmer)
	const float gain = _look.underwater ? 45.0f : 18.0f + 42.0f * _look.night;
	for (int y = 0; y < h; ++y)
	{
		Uint8 *row = getRaw(0, y);
		const float gy = std::clamp((y + 0.5f) / cell - 0.5f, 0.0f, gh - 1.001f);
		const int y0 = (int)gy;
		const float fy = gy - y0;
		const float *r0 = &_glowBuf[y0 * gw], *r1 = &_glowBuf[std::min(gh - 1, y0 + 1) * gw];
		for (int x = 0; x < w; ++x)
		{
			if (_glowMask[y * w + x] || !row[x]) continue;
			const float gx = std::clamp((x + 0.5f) / cell - 0.5f, 0.0f, gw - 1.001f);
			const int x0 = (int)gx;
			const float fx = gx - x0;
			const int x1 = std::min(gw - 1, x0 + 1);
			const float g = (r0[x0] * (1 - fx) + r0[x1] * fx) * (1 - fy) + (r1[x0] * (1 - fx) + r1[x1] * fx) * fy;
			const float v = g * gain;
			if (v <= dither(x, y)) continue;
			const int steps = v > 2.0f + dither(x + 1, y + 2) ? 3 : v > 1.0f + dither(x + 1, y + 2) ? 2 : 1;
			row[x] = (Uint8)((row[x] & 0xF0) | std::max(0, (row[x] & 15) - steps));
		}
	}
}

/**
 * Advances the particles by the time since the last frame and spawns new ones.
 */
void FirstPersonView::updateParticles()
{
	const Uint32 now = SDL_GetTicks();
	const float dt = _particleTick ? std::min<Uint32>(100, now - _particleTick) / 1000.0f : 0.0f;
	_particleTick = now;
	if (!_particlesOn)
	{
		_particles.clear();
		return;
	}
	if (Options::fpsParticles == 0)
	{
		_particles.clear();
		updateFlares(dt); // flares still fly and shine without their particles
		return;
	}
	_particles.update(dt);
	updateFlares(dt);
	spawnEffects(dt);
	spawnPartnerEffects();
	spawnDevices(dt);
	if (Options::fpsParticles > 1)
	{
		spawnAmbient(dt);
	}

}

/**
 * Draws the particles: dots (bigger up close) or streaks for rain, depth-tested against the
 * scene; self-lit ones darken as they age, the rest are lit by the battlescape light.
 */
void FirstPersonView::drawParticles()
{
	typedef FirstPersonParticles P;
	const int w = getWidth(), h = getHeight();
	const double focal = (w / 2.0) / _fovTan;
	for (const auto &p : _particles.all())
	{
		double world[3] = { p.pos[0], p.pos[1], p.pos[2] };
		if (p.kind == P::BUBBLE)
		{
			// wobble from side to side as it rises
			const float phase = p.age * 5.0f + p.pos[0] * 0.7f + p.pos[1] * 0.3f;
			world[0] += std::sin(phase) * 0.9;
			world[1] += std::cos(phase * 0.8f) * 0.9;
		}
		double sx, sy, zc;
		if (!project(world, sx, sy, zc) || sx < -4 || sy < -4 || sx >= w + 4 || sy >= h + 4)
		{
			continue;
		}
		const float fade = p.age / p.life;
		int shade;
		if (p.kind == P::LIT || p.kind == P::BUBBLE)
		{
			// bubbles and specks in the water catch and scatter what light there is: shade them less
			const float scatter = p.kind == P::BUBBLE ? 0.3f : (_look.underwater && p.group == P::WEATHER ? 0.45f : 1.0f);
			shade = (int)(std::clamp(lightAt(p.pos[0], p.pos[1], p.pos[2]), 0.0f, 15.0f) * scatter + (p.group == P::EFFECT ? fade * 4.0f : 0.0f));
		}
		else
		{
			if (p.kind == P::BLINK && std::sin(p.age * 5.0f + p.pos[0]) < 0.3f) continue;
			shade = (int)(fade * 6.0f);
		}
		const Uint8 color = shadeColor(p.color, shade);
		if (p.kind == P::STREAK)
		{
			const double tail[3] = { p.pos[0] - p.vel[0] * 0.03, p.pos[1] - p.vel[1] * 0.03, p.pos[2] - p.vel[2] * 0.03 };
			double tx, ty, tz;
			if (!project(tail, tx, ty, tz)) continue;
			const int steps = std::max(1, (int)std::max(std::abs(tx - sx), std::abs(ty - sy)));
			for (int i = 0; i <= steps; ++i)
			{
				const int x = (int)(sx + (tx - sx) * i / steps), y = (int)(sy + (ty - sy) * i / steps);
				if (x < 0 || y < 0 || x >= w || y >= h || zc >= _depth[y * w + x]) continue;
				*getRaw(x, y) = color;
			}
			continue;
		}
		const int r = std::clamp((int)std::lround(p.size * focal / zc * (p.kind == P::BUBBLE ? 0.18 : 0.12)), 1, p.kind == P::BUBBLE ? 9 : 6);
		if (p.kind == P::BUBBLE && r >= 3)
		{
			// a ring with a glint on its upper left
			const double rr = r * 0.5;
			for (int y = (int)(sy - rr) - 1; y <= (int)(sy + rr) + 1; ++y)
			{
				for (int x = (int)(sx - rr) - 1; x <= (int)(sx + rr) + 1; ++x)
				{
					if (x < 0 || y < 0 || x >= w || y >= h || zc >= _depth[y * w + x]) continue;
					const double d = std::sqrt((x + 0.5 - sx) * (x + 0.5 - sx) + (y + 0.5 - sy) * (y + 0.5 - sy));
					if (d <= rr && d > rr - 1.0)
					{
						*getRaw(x, y) = color;
					}
				}
			}
			const int gx = (int)(sx - rr * 0.35), gy = (int)(sy - rr * 0.35);
			if (gx >= 0 && gy >= 0 && gx < w && gy < h && zc < _depth[gy * w + gx])
			{
				*getRaw(gx, gy) = shadeColor(p.color, std::max(0, shade - 3));
			}
			continue;
		}
		const int x0 = (int)sx - r / 2, y0 = (int)sy - r / 2;
		for (int y = y0; y < y0 + r; ++y)
		{
			for (int x = x0; x < x0 + r; ++x)
			{
				if (r >= 3 && (x == x0 || x == x0 + r - 1) && (y == y0 || y == y0 + r - 1)) continue; // round off the corners
				if (x < 0 || y < 0 || x >= w || y >= h || zc >= _depth[y * w + x]) continue;
				*getRaw(x, y) = color;
				if (p.kind == P::GLOW || p.kind == P::BLINK) _glowMask[y * w + x] = 1; // sparks and embers bloom too
			}
		}
	}
}

/**
 * Marks the visible enemies like the iso map's indicators do: a red arrow over the head with
 * the enemy's number, drawn over everything (it's what the soldier knows, not what's in view),
 * kept at the screen edge when the enemy is off to the side.
 */
void FirstPersonView::drawMarkers()
{
	const int w = getWidth(), h = getHeight();
	const int k = std::max(1, _uiScale);
	int ax = 0, ay = 0; // the marker's point: the pixel art is drawn k times bigger around it
	auto put = [&](int x, int y, Uint8 color) {
		const int px = ax + (x - ax) * k, py = ay + (y - ay) * k;
		for (int j = 0; j < k; ++j)
			for (int i = 0; i < k; ++i)
				if (px + i >= 0 && py + j >= 0 && px + i < w && py + j < h) *getRaw(px + i, py + j) = color;
	};
	for (const auto &spot : _markerSpots)
	{
		double sx, sy, zc;
		if (!project(spot.pos, sx, sy, zc))
		{
			continue;
		}
		const int x = std::clamp((int)sx, 6 * k, w - 7 * k), y = std::clamp((int)sy, 14 * k, h - 4 * k);
		ax = x;
		ay = y;
		// arrow pointing down, 7 wide, outlined
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int row = 0; row < 4; ++row)
			{
				const int half = 3 - row + (pass == 0 ? 1 : 0);
				for (int dx = -half; dx <= half; ++dx)
				{
					put(x + dx, y - 4 + row + (pass == 0 ? (row == 3 ? 1 : 0) : 0), pass == 0 ? _look.tapeBlack : _look.marker);
				}
			}
		}
		put(x, y, _look.tapeBlack);
		// number above it
		const std::string digits = std::to_string(spot.number);
		const int width = (int)digits.size() * 4 - 1;
		const int x0 = x - width / 2, y0 = y - 11;
		for (int pass = 0; pass < 2; ++pass)
		{
			for (size_t i = 0; i < digits.size(); ++i)
			{
				const Uint8 *glyph = DigitFont[digits[i] - '0'];
				for (int row = 0; row < 5; ++row)
				{
					for (int col = 0; col < 3; ++col)
					{
						if (!(glyph[row] & (4 >> col))) continue;
						const int gx = x0 + (int)i * 4 + col, gy = y0 + row;
						if (pass == 0)
						{
							for (int oy = -1; oy <= 1; ++oy)
								for (int ox = -1; ox <= 1; ++ox)
									put(gx + ox, gy + oy, _look.tapeBlack);
						}
						else
						{
							put(gx, gy, _look.marker);
						}
					}
				}
			}
		}
	}
}

/**
 * Renders the view: terrain, border and sky by ray casting (spread over a few threads),
 * then units, items and effects.
 */
void FirstPersonView::draw()
{
	if (!_save || !_cameraUnit)
	{
		Surface::draw();
		return;
	}
	_redraw = false;

	// camera shake: the eye jitters for this frame only, dying away
	const Uint32 shakeNow = SDL_GetTicks();
	const float shakeDt = _shakeTick ? std::min<Uint32>(100, shakeNow - _shakeTick) / 1000.0f : 0.0f;
	_shakeTick = shakeNow;
	if (_shake > 0.02f)
	{
		// smooth shaking (a few wobbles a second), not a new random jolt every frame
		const float t = shakeNow / 1000.0f;
		_eye[0] += _shake * 0.6f * std::sin(t * 41.0f) * std::cos(t * 13.0f);
		_eye[1] += _shake * 0.6f * std::sin(t * 37.0f + 1.3f);
		_eye[2] += _shake * std::sin(t * 47.0f + 2.1f) * std::cos(t * 11.0f);
		_shake *= std::exp(-shakeDt * 7.0f);
	}
	else
	{
		_shake = 0.0f;
	}
	_kick *= std::exp(-shakeDt * 9.0f);
	if (_kick < 0.02f) _kick = 0.0f;
	const Uint32 sinceFlash = SDL_GetTicks() - _muzzleTick;
	_muzzleLight = Options::fpsMuzzleFlash && _muzzleTick && sinceFlash < 90 ? 1.0f - sinceFlash / 90.0f : 0.0f;

	updateBasis();
	// the flashlight sits on the right shoulder, a little below the eye
	for (int a = 0; a < 3; ++a)
	{
		_flash[a] = (float)(_eye[a] + _right[a] * 4.0 - _up[a] * 4.0 + _fwd[a] * 2.0);
	}
	snapshotTiles();
	if (!_groundChosen)
	{
		chooseGround();
	}
	updateLook();
	updateParticles();

	lock();
	static const int threads = std::clamp((int)std::thread::hardware_concurrency(), 1, 8);
	std::vector<std::thread> pool;
	for (int i = 1; i < threads; ++i)
	{
		pool.emplace_back(&FirstPersonView::renderRows, this, i, threads);
	}
	renderRows(0, threads);
	for (auto &thread : pool)
	{
		thread.join();
	}
	watchUnits();
	drawDecals();
	drawFloorItems();
	drawUnits();
	drawSmokeAndFire();
	drawEffects();
	drawFlares();
	drawBeams();
	drawParticles();
	drawHalos();
	// night vision recolours everything into one colour group, keeping each shade (like the map)
	if (_nightVision || _tint > 0.0f || _vignette > 0.0f || _static > 0.0f)
	{
		const int w = getWidth(), h = getHeight();
		const float cx = w * 0.5f, cy = h * 0.5f, reach = std::sqrt(cx * cx + cy * cy);
		const Uint8 group = _nightVision ? (Uint8)((_nightVision - 1) * 16) : 0;
		const Uint8 *wash = _tintColor == 1 ? _look.blueTint : _tintColor == 2 ? _look.orangeTint : _tintColor == 3 ? _look.whiteTint
			: _tintColor == 4 ? _look.greenTint : _look.redTint;
		for (int y = 0; y < h; ++y)
		{
			Uint8 *row = getRaw(0, y);
			for (int x = 0; x < w; ++x)
			{
				if (_nightVision && row[x]) row[x] = group | (row[x] & 15);
				if (_tint > 0.0f && dither(x, y) < _tint) row[x] = (_tintRed > 0.0f && dither(y + 1, x + 2) < _tintRed ? _look.redTint : wash)[row[x]];
				if (_static > 0.0f)
				{
					_staticSeed = _staticSeed * 1664525u + 1013904223u;
					if ((_staticSeed >> 8 & 255) < _static * 256.0f) row[x] = _look.grey[_staticSeed >> 30];
				}
				if (_vignette > 0.0f)
				{
					const float r = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / reach;
					if (smoothstep(0.55f, 1.0f, r) * _vignette > dither(x + 3, y + 1)) row[x] = _look.redTint[row[x]];
				}
			}
		}
	}
	drawBurning();
	drawHurt();
	drawMarkers();
	unlock();
}

}
