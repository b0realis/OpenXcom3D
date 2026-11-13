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
 * FirstPersonViewGpu.cpp: the view's rays on the graphics card, run beside the CPU's.
 *
 * FirstPersonViewGpu.cpp: the view's rays on the graphics card (setting "Renderer": GPU).
 *
 * The card casts the rays castAllRays would (FirstPersonGpu: the same walk and the same shading,
 * as a shader) and its picture, depth and glow land in the view's own buffers, so everything
 * drawn after the rays (units, items, smoke, effects, the HUD) works on it unchanged. Where the
 * card can't be had, or fails, the CPU draws as ever.
 *
 * The CPU renderer is the reference. With OPENXCOM3D_GPU_SHADOW set the CPU draws the picture
 * and the card casts the same rays beside it: every thirtieth frame each ray is checked against
 * the CPU's own castRay and each colour against the CPU's picture, and the log gets both
 * renderers' times and the counts that differ. (OPENXCOM3D_GPU_DEVICE: the card, by number;
 * OPENXCOM3D_GPU_SCALE: the shadow's picture so many times the view's size, for timing;
 * OPENXCOM3D_GPU_DUMP: a folder for both pictures of the last check.)
 */
#include "FirstPersonView.h"
#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <set>
#include <sstream>
#include <thread>
#include "../Engine/Logger.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Projectile.h"
#include "../Battlescape/Explosion.h"
#include "../Savegame/Tile.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonGpu.h"
#include "FirstPersonViewShared.h"
#include "FirstPersonViewRayWalk.h"

namespace OpenXcom
{

namespace
{

/// The one renderer on the card. Opening it takes a moment (the driver builds the shaders: seconds, the
/// first time), so it is opened on the side the first time a view asks, and is null until it is
/// ready (the CPU draws meanwhile), and for good where there is none (asked for by number in
/// OPENXCOM3D_GPU_DEVICE: that card or none).
struct SharedGpu
{
	std::unique_ptr<FirstPersonGpu> gpu;
	std::atomic<FirstPersonGpu*> ready { nullptr };
	std::thread opening;
	bool tried = false;
	~SharedGpu()
	{
		if (opening.joinable()) opening.join();
	}
};

FirstPersonGpu *sharedGpu(bool open)
{
	static SharedGpu shared;
	if (open && !shared.tried)
	{
		shared.tried = true;
		shared.opening = std::thread([] {
			const char *device = getenv("OPENXCOM3D_GPU_DEVICE");
			shared.gpu = device ? FirstPersonGpu::open(atoi(device)) : FirstPersonGpu::openAny();
			// (The programs for "Coloured lights", "Reflections" and "Light shafts", if those are on, before the card is called ready.)
			if (shared.gpu) shared.gpu->prebuild((Options::fpsTrueColor && Options::fpsColoredLights ? 1 : 0) | (Options::fpsTrueColor && Options::fpsReflections ? 2 : 0) | (Options::fpsTrueColor && Options::fpsLightShafts ? 8 : 0)
				| (Options::fpsSupersampling > 0 ? 4 : 0));
			shared.ready = shared.gpu.get();
		});
	}
	return shared.ready.load();
}

}

FirstPersonGpu *readyFirstPersonGpu()
{
	return sharedGpu(false);
}

namespace
{

/// Sums over the shadow run's frames since its last line in the log.
struct ShadowStats
{
	int frames = 0;
	double cpu = 0.0, upload = 0.0, draw = 0.0, read = 0.0;
	long checkedRays = 0, differentRays = 0;
	long checkedPixels = 0, differentPixels = 0;
	int detailed = 0; // differing rays written out in full so far
};

}

/**
 * Reads the view's state for the card (the view's private parts, hence a friend of it).
 */
struct FirstPersonViewGpuAccess
{
	static void fillFrame(const FirstPersonView &view, FirstPersonGpu::Frame &frame, int scale);
};

/**
 * A frame for the card as renderRows and shadeHit have it: the world, the camera, the scene.
 * @param scale The picture so many times the view's own size each way (the shadow run's timing); 1: the view's.
 */
void FirstPersonViewGpuAccess::fillFrame(const FirstPersonView &view, FirstPersonGpu::Frame &frame, int scale)
{
	const int width = view.getWidth() * scale, height = view.getHeight() * scale;
	// The frame as renderRows has it.
	frame.tiles = view._tileViews.data();
	frame.tileFlags = view._tileFlags.data();
	frame.swings = view._doorSwings.data();
	frame.swingCount = (int)view._doorSwings.size();
	frame.size[0] = view._save->getMapSizeX();
	frame.size[1] = view._save->getMapSizeY();
	frame.size[2] = view._save->getMapSizeZ();
	frame.maxDistance = view._maxDistance;
	for (int axis = 0; axis < 3; ++axis)
	{
		frame.eye[axis] = (float)view._eye[axis];
		frame.forward[axis] = (float)view._forward[axis];
		frame.right[axis] = (float)view._right[axis];
		frame.up[axis] = (float)view._up[axis];
	}
	frame.halfPlaneWidth = (float)view._halfFovTangent;
	frame.halfPlaneHeight = (float)(view._halfFovTangent * height / width);
	// An eye's picture in VR (see FirstPersonView::useEyeCamera): its image plane as renderRows has it for an eye.
	frame.eyeAxis[0] = view._eyePass ? (float)view._eyeAxisX * scale : 0.0f;
	frame.eyeAxis[1] = view._eyePass ? (float)view._eyeAxisY * scale : 0.0f;
	frame.eyeFocal = view._eyePass ? (float)view._eyeFocal * scale : 0.0f;
	frame.width = width;
	frame.height = height;
	// (Auto on the card: a ray a pixel whatever the view's size, which is what the card is for; a small view always has one,
	// and so has an eye's picture.)
	frame.pixelSize = scale > 1 || view._eyePass || width * height <= SmallViewPixels || Options::fpsPixelSize == 0 ? 1 : std::clamp(Options::fpsPixelSize, 1, 3);
	frame.ghostScale = std::clamp(Options::fpsGhostOpacity, 10, 100) / (GhostNearOpacity * 100.0f);
	frame.seeThroughCrops = Options::fpsSeeThroughCrops;
	// The scene, as shadeHit and colorOutsideMap read it.
	static std::vector<Uint8> roofed;
	roofed.resize(view._tileLights.size());
	for (size_t index = 0; index < roofed.size(); ++index) roofed[index] = view._tileLights[index].roofed;
	frame.colors = &view._colors;
	frame.roofed = roofed.data();
	frame.waterAtEdge = view._waterModel && !view._waterAtEdge.empty() ? view._waterAtEdge.data() : nullptr;
	frame.groundModel = view._groundModel;
	frame.waterModel = view._waterModel;
	std::copy(view._waterColors, view._waterColors + 3, frame.waterColors);
	frame.rockBase = view._rockBase;
	frame.sky = (int)view._sky;
	frame.enclosed = view._enclosed;
	frame.surfaceLevel = view._surfaceLevel;
	std::copy(&view._waveSteps[0][0], &view._waveSteps[0][0] + 18, frame.waveSteps);
	frame.animationBlend = view._animationBlend;
	frame.sightView = view._save->getTileEngine()->getMaxViewDistance() * (float)Position::TileXY;
	frame.sightMod = view._save->getMod()->getMaxViewDistance() * (float)Position::TileXY;
	frame.fogOfWar = Options::fpsFogOfWar;
	frame.unseenTerrain = Options::fpsUnseenTerrain;
	frame.haze = Options::fpsHaze;
	frame.shadows = Options::fpsShadows;
	frame.smoothLighting = view._smoothLighting;
	frame.discreetEdge = Options::fpsDiscreetEdge;
	frame.samples = Options::fpsSupersampling >= 2 ? 4 : Options::fpsSupersampling == 1 ? 2 : 1;
	frame.softShadows = Options::fpsSoftShadows;
	frame.allLightShadows = Options::fpsAllLightShadows;
	frame.ambientOcclusion = Options::fpsAmbientOcclusion;
	frame.palette = view.getPalette();
	frame.smoothLight = Options::fpsSmoothLight && view._wantsTrueColor;
	frame.reflections = Options::fpsReflections && view._wantsTrueColor && view._waterColors[0] != 0; // (only where there is a sea)
	// The coloured lights (setting "Coloured lights", in full colour): the shots in flight (but plain bullets), the
	// explosions, the muzzle flash, and the nearest fires; sixteen at most, in that order.
	frame.colorLightCount = 0;
	frame.colorLightsOn = Options::fpsColoredLights && view._wantsTrueColor;
	if (Options::fpsColoredLights && view._wantsTrueColor && view._map)
	{
		const SDL_Color *palette = view.getPalette();
		auto add = [&](float x, float y, float z, float reach, float red, float green, float blue, float strength)
		{
			if (frame.colorLightCount >= 16 || strength <= 0.0f) return;
			const float light[8] = { x, y, z, reach, red / 255.0f, green / 255.0f, blue / 255.0f, strength };
			std::copy(light, light + 8, frame.colorLights[frame.colorLightCount++]);
		};
		// A shot's colour: its trail's, made as bright as it goes (a dark red bolt still lights red).
		auto addShot = [&](const float *head, FirstPersonView::Trail trail, Uint8 color, float calibre)
		{
			if (trail == FirstPersonView::TRAIL_NONE || trail == FirstPersonView::TRAIL_FLYING || trail == FirstPersonView::TRAIL_SMOKE) return;
			float red = 255, green = 150, blue = 60; // (a rocket's flame, fire)
			if (trail != FirstPersonView::TRAIL_FIRE && color)
			{
				const float top = std::max({ 1, (int)palette[color].r, (int)palette[color].g, (int)palette[color].b });
				red = palette[color].r * 255.0f / top; green = palette[color].g * 255.0f / top; blue = palette[color].b * 255.0f / top;
			}
			add(head[0], head[1], head[2], 72.0f + 12.0f * std::min(3.0f, calibre), red, green, blue, 0.9f);
		};
		// (OPENXCOM3D_GPU_TESTLIGHT: an orange light three tiles ahead of the eye, to look at the lights without a fire at hand.)
		static const bool testLight = getenv("OPENXCOM3D_GPU_TESTLIGHT") != nullptr;
		if (testLight) add((float)(view._eye[0] + view._forward[0] * 48.0), (float)(view._eye[1] + view._forward[1] * 48.0), (float)view._eye[2], 120.0f, 255, 130, 45, 1.0f);
		if (Projectile *projectile = view.flyingProjectile())
		{
			if (!projectile->getItem())
			{
				float head[3];
				view.shotHead(projectile, head);
				addShot(head, view._shotTrail, view._shotTrailColor, view._shotCalibre);
			}
		}
		if (view._map->getPartnerProjectile()) addShot(view._partnerHead, view._partnerTrail, view._partnerColor, view._partnerCalibre);
		for (const Explosion *explosion : *view._map->getExplosions())
		{
			const int step = explosion->getCurrentFrame();
			if (step < 0) continue;
			const Position at = explosion->getPosition();
			const float fading = std::max(0.15f, 1.0f - step / 8.0f);
			if (explosion->isBig()) add((float)at.x, (float)at.y, (float)at.z + 6.0f, 176.0f, 255, 170, 70, 1.6f * fading);
			else add((float)at.x, (float)at.y, (float)at.z, 44.0f, 255, 215, 150, 0.6f * fading);
		}
		if (view._muzzleFlashLight > 0.0f) add(view._muzzleFlashPos[0], view._muzzleFlashPos[1], view._muzzleFlashPos[2], 96.0f, 255, 220, 150, 0.5f * view._muzzleFlashLight);
		// The fires nearest the eye, each flickering by its own clock.
		struct Fire { float distance; int tile; };
		Fire nearest[8];
		int fires = 0;
		const int tileCount = view._save->getMapSizeXYZ();
		for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
		{
			Tile *tile = view._save->getTile(tileIndex);
			if (!tile->getFire() || !view._tileViews[tileIndex].discovered) continue;
			const Position place = tile->getPosition();
			const float dx = place.x * 16.0f + 8.0f - (float)view._eye[0], dy = place.y * 16.0f + 8.0f - (float)view._eye[1], dz = place.z * 24.0f + 8.0f - (float)view._eye[2];
			const Fire fire = { dx * dx + dy * dy + dz * dz, tileIndex };
			int slot;
			if (fires < 8) slot = fires++;
			else if (fire.distance < nearest[7].distance) slot = 7;
			else continue;
			while (slot > 0 && nearest[slot - 1].distance > fire.distance) { nearest[slot] = nearest[slot - 1]; --slot; }
			nearest[slot] = fire;
		}
		const float seconds = SDL_GetTicks() / 1000.0f;
		for (int fire = 0; fire < fires; ++fire)
		{
			Tile *tile = view._save->getTile(nearest[fire].tile);
			const Position place = tile->getPosition();
			const float flicker = 0.5f + 0.12f * std::sin(seconds * 9.0f + tile->getAnimationOffset() * 1.7f) + 0.06f * std::sin(seconds * 23.0f + nearest[fire].tile);
			add(place.x * 16.0f + 8.0f, place.y * 16.0f + 8.0f, place.z * 24.0f - tile->getTerrainLevel() + 8.0f, 60.0f, 255, 135, 45, flicker);
		}
	}
	frame.lampCount = (int)std::min<size_t>(16, view._nearbyLights.size());
	for (int lamp = 0; lamp < 16; ++lamp)
	{
		for (int axis = 0; axis < 3; ++axis) frame.lamps[lamp][axis] = lamp < frame.lampCount ? view._nearbyLights[lamp].pos[axis] : 0.0f;
		frame.lamps[lamp][3] = lamp < frame.lampCount ? view._nearbyLights[lamp].strength : 0.0f;
	}
	frame.flashlight = !view._flashlight ? 0 : view._dynamicFlashlight ? 2 : 1;
	frame.flareCount = (int)std::min<size_t>(8, view._flareLights.size());
	for (int flare = 0; flare < 8; ++flare)
	{
		const bool burning = flare < frame.flareCount;
		for (int axis = 0; axis < 3; ++axis) frame.flares[flare][axis] = burning ? view._flareLights[flare].pos[axis] : 0.0f;
		frame.flares[flare][3] = burning ? view._flareLights[flare].fade : 0.0f;
		frame.flareFlicker[flare] = burning ? view._flareLights[flare].flicker : 0.0f;
	}
	// Light shafts (setting "Light shafts", in full colour): only in a frame that has what shows them, smoke somewhere
	// on the map or the sea all round, and a light to show (else the plain program draws, at its own speed).
	frame.lightShafts = frame.shaftSmoke = false;
	frame.shaftAir = view._colors.underwater ? 0.0005f : 0.0f;
	if (Options::fpsLightShafts && view._wantsTrueColor)
	{
		const size_t tileCount = (size_t)view._save->getMapSizeXYZ();
		for (size_t tile = 0; tile < tileCount && !frame.shaftSmoke; ++tile)
		{
			frame.shaftSmoke = view._tileViews[tile].smoke != 0;
		}
		const bool lampsShine = frame.lampCount > 0 && (view._colors.underwater || view._colors.night > 0.3f);
		frame.lightShafts = (frame.shaftSmoke || view._colors.underwater) && (frame.flashlight || frame.flareCount > 0 || lampsShine || frame.colorLightCount > 0);
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		frame.flashlightPos[axis] = view._flashlightPos[axis];
		frame.forwardInVoxels[axis] = (float)view._forwardInVoxels[axis];
		frame.muzzleFlashPos[axis] = view._muzzleFlashPos[axis];
	}
	frame.muzzleFlashLight = view._muzzleFlashLight;
	// UFO lights, crafts' mirrors, wall paints.
	const size_t tileCount = view._tileViews.size();
	frame.ufoLightsOn = view._ufoLightsOn;
	frame.ufoOfTile = view._ufoOfTile.size() == tileCount ? view._ufoOfTile.data() : nullptr;
	frame.ufoPhaseOfTile = view._ufoPhaseOfTile.size() == tileCount ? view._ufoPhaseOfTile.data() : nullptr;
	std::copy(&view._ufoPulse[0][0], &view._ufoPulse[0][0] + 32, frame.ufoPulse);
	frame.ufoBreath = view._ufoBreath;
	std::copy(view._ufoFaceShade, view._ufoFaceShade + 64, frame.ufoFaceShade);
	frame.ufoCount = (int)view._ufos.size(); // (more than eight: the CPU draws, see FirstPersonGpu's limits)
	for (int ufo = 0; ufo < std::min(frame.ufoCount, 8); ++ufo)
	{
		frame.ufoTint[ufo] = &view._ufos[ufo].tint[0][0];
		frame.ufoSoftTint[ufo] = &view._ufos[ufo].softTint[0][0];
	}
	const SDL_Color *gamePalette = view.getPalette();
	for (int color = 0; color < 256; ++color)
	{
		frame.black[color] = gamePalette && gamePalette[color].r * 299 + gamePalette[color].g * 587 + gamePalette[color].b * 114 < 14000;
	}
	static std::vector<FirstPersonGpu::WallPaint> wallPaints;
	wallPaints.clear();
	for (const FirstPersonView::WallPaint &paint : view._wallPaints) wallPaints.push_back({ paint.wall, paint.donor, paint.donorWest, paint.openEnd, paint.kind, paint.paintGroup });
	frame.wallPaint = Options::fpsWallPaint;
	frame.wallPaintOf = view._wallPaintOf.size() == tileCount * 2 ? view._wallPaintOf.data() : nullptr;
	frame.wallPaints = wallPaints.data();
	frame.wallPaintCount = (int)wallPaints.size();
	static std::vector<FirstPersonGpu::Mirror> mirrors;
	mirrors.clear();
	for (const FirstPersonView::Mirror &mirror : view._mirrors)
	{
		FirstPersonGpu::Mirror sent;
		std::copy(mirror.lo, mirror.lo + 3, sent.lo);
		std::copy(mirror.size, mirror.size + 3, sent.size);
		for (int axis = 0; axis < 2; ++axis)
		{
			sent.sum[axis] = mirror.sum[axis];
			sent.symmetric[axis] = mirror.symmetric[axis];
			sent.start[axis] = mirror.start[axis].data();
			sent.startCount[axis] = mirror.start[axis].size();
			sent.pos[axis] = mirror.pos[axis].data();
			sent.posCount[axis] = mirror.pos[axis].size();
			sent.outer[axis] = mirror.outer[axis].data();
			sent.outerCount[axis] = mirror.outer[axis].size();
		}
		mirrors.push_back(sent);
	}
	frame.mirrorOfTile = view._mirrorOfTile.size() == tileCount ? view._mirrorOfTile.data() : nullptr;
	frame.mirrors = mirrors.data();
	frame.mirrorCount = (int)mirrors.size();
}

void FirstPersonView::gpuWarmUp()
{
	static const bool gpuShadow = getenv("OPENXCOM3D_GPU_SHADOW") != nullptr;
	if (gpuShadow || Options::fpsRenderer == 1)
	{
		sharedGpu(true);
	}
}

void FirstPersonView::gpuForgetModels()
{
	if (FirstPersonGpu *gpu = sharedGpu(false))
	{
		gpu->forgetModels();
	}
}

/**
 * Casts this frame's rays on the graphics card (setting "Renderer": GPU), into the view's own
 * picture, depth buffer and glow mask.
 * @return False where the card can't be had or failed: the caller casts them on the CPU.
 */
bool FirstPersonView::gpuCastAllRays()
{
	FirstPersonGpu *gpu = sharedGpu(true);
	if (!gpu)
	{
		return false;
	}
	FirstPersonGpu::Frame frame;
	FirstPersonViewGpuAccess::fillFrame(*this, frame, 1);
	FirstPersonGpu::Output output;
	output.color = getRaw(0, 0);
	output.colorPitch = getPitch();
	output.depth = _depthBuffer.data();
	output.glow = _glowMask.data();
	output.results = nullptr;
	output.perRay = false;
	// The units and effects on the card too: the rays' pictures stay there, and come back finished (see gpuDrawEffects).
	output.keep = gpuEffectsWanted();
	_gpuRaysPixelSize = frame.pixelSize;
	// The picture in full colour too, if the mode shows this view that way (setting "True colour"; a ray a pixel).
	const bool fullColor = _wantsTrueColor && frame.pixelSize == 1;
	if (fullColor) _trueColor.resize((size_t)getWidth() * getHeight());
	output.trueColor = fullColor ? _trueColor.data() : nullptr;
	// (OPENXCOM3D_GPU_TIMING: where the card's time goes, in the log every 60 frames: sending, drawing, reading back.
	// The card is waited for after each step, so the frame is slower than it is without.)
	static const bool timed = getenv("OPENXCOM3D_GPU_TIMING") != nullptr;
	FirstPersonGpu::Timing timing;
	// Each kind of frame the settings can ask for is drawn once before the first frame that is shown: with a coloured
	// light, with light in the air, with both. A driver finishes a program the first time something is drawn with
	// it (Apple's takes a second or more over each), and the game would stand still at the first shot, the first
	// lamp in smoke. So it waits here instead, where a battle begins. The real frame is drawn over these.
	{
		static std::set<int> warmed; // (the card is one for every view)
		const bool shaftsOn = Options::fpsLightShafts && _wantsTrueColor;
		const int settings = (frame.reflections ? 4 : 0) | frame.samples << 3 | (frame.pixelSize == 1 ? 64 : 0) | (output.keep ? 128 : 0);
		const auto start = std::chrono::steady_clock::now();
		int drawn = 0;
		for (int kind = 0; kind < 4; ++kind)
		{
			const bool light = (kind & 1) != 0, shafts = (kind & 2) != 0;
			if ((light && !frame.colorLightsOn) || (shafts && !shaftsOn) || !warmed.insert(settings | kind).second)
			{
				continue;
			}
			FirstPersonGpu::Frame like = frame;
			if (light && !like.colorLightCount)
			{
				std::fill(like.colorLights[0], like.colorLights[0] + 8, 0.0f); // (a light of no strength)
				like.colorLightCount = 1;
			}
			else if (!light) like.colorLightCount = 0;
			like.lightShafts = shafts;
			if (!gpu->castRays(like, output, nullptr))
			{
				return false;
			}
			++drawn;
		}
		const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (drawn && milliseconds >= 50.0)
		{
			Log(LOG_INFO) << "OpenXcom3D GPU: " << drawn << " kinds of frame drawn ahead in " << (int)milliseconds << " ms";
		}
	}
	if (!gpu->castRays(frame, output, timed ? &timing : nullptr))
	{
		return false;
	}
	if (timed && getWidth() >= 320)
	{
		static double upload = 0.0, draw = 0.0, read = 0.0;
		static int frames = 0;
		upload += timing.upload; draw += timing.draw; read += timing.read;
		if (++frames == 60)
		{
			Log(LOG_INFO) << "OpenXcom3D GPU: the " << getWidth() << "x" << getHeight() << " view: sending " << upload / frames << " ms, drawing " << draw / frames
				<< " ms, reading back " << read / frames << " ms";
			upload = draw = read = 0.0;
			frames = 0;
		}
	}
	// The rays' palette colours are kept beside it: a pixel that still has its ray's colour when the frame is done
	// (nothing was drawn over it, no wash went over it) is shown in its full colour (see trueColor).
	if (output.keep)
	{
		_hasLightShafts = frame.lightShafts;
		gpuBeginEffects();
		_gpuChecking = output.keep == 2;
		_gpuRecordOnly = !_gpuChecking;
	}
	if (output.keep != 1 && fullColor)
	{
		_rayColors.resize(_trueColor.size());
		for (int row = 0; row < getHeight(); ++row)
		{
			std::memcpy(&_rayColors[(size_t)row * getWidth()], getRaw(0, row), getWidth());
		}
		_hasTrueColor = true;
		_hasLightShafts = frame.lightShafts;
	}
	// (a line in the log the first time each view is drawn this way, as the CPU's threads have theirs)
	if (!_gpuLogged)
	{
		_gpuLogged = true;
		Log(LOG_INFO) << "OpenXcom3D GPU: the " << getWidth() << "x" << getHeight() << " 3D view's rays are cast on " << gpu->name();
	}
	return true;
}

/**
 * The shadow run: casts this frame's rays on the graphics card too, times them against the
 * CPU's, and on every thirtieth frame checks each ray against the CPU's castRay (how it ended,
 * the tile, part and voxel hit, the face, glass and glow, the distance) and each colour against
 * the CPU's picture.
 * @param cpuMilliseconds What the CPU's rays (walk and shading) took this frame.
 */
void FirstPersonView::gpuShadowRays(double cpuMilliseconds)
{
	static ShadowStats stats;
	static std::vector<Uint8> colors, glows;
	static std::vector<float> depths;
	static std::vector<FirstPersonGpu::RayResult> results;
	static const char *forcedScale = getenv("OPENXCOM3D_GPU_SCALE");
	const int scale = forcedScale ? std::max(1, atoi(forcedScale)) : 1;
	const int width = getWidth() * scale, height = getHeight() * scale;
	FirstPersonGpu *gpu = getWidth() >= 320 ? sharedGpu(true) : nullptr; // the main view only, not the small camera pictures
	if (!gpu)
	{
		return;
	}
	FirstPersonGpu::Frame frame;
	FirstPersonViewGpuAccess::fillFrame(*this, frame, scale);
	// (a ray each here, whatever the pixel size: the check goes ray by ray)
	const int pixelSize = frame.pixelSize;
	const int blockColumns = (width + pixelSize - 1) / pixelSize, blockRows = (height + pixelSize - 1) / pixelSize;
	colors.resize((size_t)blockColumns * blockRows);
	glows.resize(colors.size());
	depths.resize(colors.size());
	results.resize(colors.size());

	FirstPersonGpu::Timing timing;
	const bool check = stats.frames % 30 == 0;
	FirstPersonGpu::Output output = { colors.data(), blockColumns, depths.data(), glows.data(), check ? results.data() : nullptr, nullptr, true };
	if (!gpu->castRays(frame, output, &timing))
	{
		return;
	}
	stats.cpu += cpuMilliseconds;
	stats.upload += timing.upload;
	stats.draw += timing.draw;
	stats.read += timing.read;

	// Every ray against the CPU's.
	if (check)
	{
		RayContext rayContext;
		rayContext.tiles = frame.tiles;
		rayContext.tileFlags = frame.tileFlags;
		rayContext.swings = frame.swings;
		std::copy(frame.size, frame.size + 3, rayContext.size);
		rayContext.maxDistance = frame.maxDistance;
		rayContext.solidGhostThreshold = 0.0f;
		for (int blockRow = 0; blockRow < blockRows; ++blockRow)
		{
			const bool eyePlane = frame.eyeFocal > 0.0f; // (an eye's picture in VR: its off-centre plane, as renderRows has it)
			const float planeV = eyePlane ? (frame.eyeAxis[1] - (blockRow * frame.pixelSize + frame.pixelSize * 0.5f)) / frame.eyeFocal
				: (1.0f - (blockRow * frame.pixelSize + frame.pixelSize * 0.5f) * 2.0f / height) * frame.halfPlaneHeight;
			for (int blockColumn = 0; blockColumn < blockColumns; ++blockColumn)
			{
				const float planeU = eyePlane ? ((blockColumn * frame.pixelSize + frame.pixelSize * 0.5f) - frame.eyeAxis[0]) / frame.eyeFocal
					: ((blockColumn * frame.pixelSize + frame.pixelSize * 0.5f) * 2.0f / width - 1.0f) * frame.halfPlaneWidth;
				float rayDirection[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					rayDirection[axis] = frame.forward[axis] + planeU * frame.right[axis] + planeV * frame.up[axis];
				}
				rayDirection[2] /= ZScaleF;
				const float directionLength = std::sqrt(rayDirection[0] * rayDirection[0] + rayDirection[1] * rayDirection[1] + rayDirection[2] * rayDirection[2]);
				for (int axis = 0; axis < 3; ++axis)
				{
					rayDirection[axis] /= directionLength;
				}
				Hit hit;
				hit.color = 0;
				hit.t = 0.0f;
				hit.axis = -1;
				hit.tile = 0;
				hit.part = 0;
				hit.voxel[0] = hit.voxel[1] = hit.voxel[2] = 0;
				float endDistance = 0.0f;
				int endAxis = -1;
				rayContext.ghostThreshold = dither(blockColumn, blockRow) / frame.ghostScale;
				if (frame.seeThroughCrops) rayContext.solidGhostThreshold = dither(blockColumn, blockRow) / SeeThroughCropOpacity;
				const RayEnd rayEnd = castRay(rayContext, frame.eye, rayDirection, hit, endDistance, endAxis);

				// The card's ray, unpacked (see FirstPersonGpu::RayResult).
				const size_t index = (size_t)blockRow * blockColumns + blockColumn;
				const FirstPersonGpu::RayResult &result = results[index];
				const int gpuEnd = (result.bits >> 12) & 7, gpuPart = result.bits & 7, gpuAxis = (int)((result.bits >> 4) & 15) - 1;
				const bool gpuGhost = (result.bits >> 8) & 1, gpuGlass = (result.bits >> 9) & 1, gpuEmissive = (result.bits >> 10) & 1;
				const float cpuDistance = rayEnd == RAY_HIT ? hit.t : endDistance;
				bool same = gpuEnd == (int)rayEnd && gpuGlass == hit.glass
					&& (std::fabs(result.distance - cpuDistance) <= 0.01f + cpuDistance * 1e-4f || (std::isinf(result.distance) && std::isinf(cpuDistance)));
				if (same && rayEnd == RAY_HIT)
				{
					same = result.tile == (Uint32)hit.tile && gpuPart == (hit.part < 0 ? 7 : hit.part) && gpuAxis == hit.axis && gpuGhost == hit.ghost
						&& gpuEmissive == hit.emissive && ((result.bits >> 16) & 255) == hit.color
						&& (hit.part < 0 || result.voxel == (Uint32)(hit.voxel[0] | hit.voxel[1] << 8 | hit.voxel[2] << 16));
				}
				else if (same)
				{
					same = gpuAxis == endAxis;
				}
				++stats.checkedRays;
				if (!same)
				{
					++stats.differentRays;
					if (stats.detailed < 12)
					{
						++stats.detailed;
						Log(LOG_INFO) << "OpenXcom3D GPU: ray " << blockColumn << "," << blockRow << " differs: CPU end " << rayEnd << " tile " << hit.tile << " part " << hit.part
							<< " axis " << (rayEnd == RAY_HIT ? hit.axis : endAxis) << " voxel " << (int)hit.voxel[0] << "," << (int)hit.voxel[1] << "," << (int)hit.voxel[2]
							<< " colour " << (int)hit.color << " glass " << hit.glass << " glow " << hit.emissive << " at " << cpuDistance
							<< "; GPU end " << gpuEnd << " tile " << result.tile << " part " << gpuPart << " axis " << gpuAxis << " voxel " << (result.voxel & 255) << ","
							<< ((result.voxel >> 8) & 255) << "," << (result.voxel >> 16) << " colour " << ((result.bits >> 16) & 255) << " glass " << gpuGlass << " glow " << gpuEmissive
							<< " at " << result.distance;
					}
				}
			}
		}
	}

	// The finished colours against the CPU's picture (the same size only: the view's own rays).
	if (check && scale == 1)
	{
		long different = 0;
		for (int blockRow = 0; blockRow < blockRows; ++blockRow)
		{
			const Uint8 *cpuRow = getRaw(0, blockRow * frame.pixelSize);
			for (int blockColumn = 0; blockColumn < blockColumns; ++blockColumn)
			{
				different += cpuRow[blockColumn * frame.pixelSize] != colors[(size_t)blockRow * blockColumns + blockColumn];
			}
		}
		stats.checkedPixels += (long)blockColumns * blockRows;
		stats.differentPixels += different;
		// (OPENXCOM3D_GPU_DUMP: a folder for both pictures of the last check, as raw palette bytes, and the palette.)
		static const char *dumpFolder = getenv("OPENXCOM3D_GPU_DUMP");
		if (dumpFolder)
		{
			std::vector<Uint8> cpuPicture((size_t)blockColumns * blockRows);
			for (int blockRow = 0; blockRow < blockRows; ++blockRow)
			{
				for (int blockColumn = 0; blockColumn < blockColumns; ++blockColumn)
				{
					cpuPicture[(size_t)blockRow * blockColumns + blockColumn] = *getRaw(blockColumn * frame.pixelSize, blockRow * frame.pixelSize);
				}
			}
			Uint8 palette[768];
			for (int index = 0; index < 256; ++index)
			{
				palette[index * 3] = getPalette()[index].r;
				palette[index * 3 + 1] = getPalette()[index].g;
				palette[index * 3 + 2] = getPalette()[index].b;
			}
			const std::string folder = dumpFolder;
			auto write = [&](const char *name, const void *data, size_t size) {
				if (FILE *file = fopen((folder + "/" + name).c_str(), "wb")) { fwrite(data, 1, size, file); fclose(file); }
			};
			const int size[2] = { blockColumns, blockRows };
			write("size.bin", size, sizeof(size));
			write("palette.bin", palette, sizeof(palette));
			write("cpu.bin", cpuPicture.data(), cpuPicture.size());
			write("gpu.bin", colors.data(), colors.size());
		}
	}

	// A line in the log every sixty frames.
	if (++stats.frames % 60 == 0)
	{
		std::ostringstream line;
		line.setf(std::ios::fixed);
		line.precision(2);
		line << "OpenXcom3D GPU: " << blockColumns << "x" << blockRows << " rays, reach " << (int)(frame.maxDistance / 16) << " tiles: CPU rays " << stats.cpu / 60
			<< " ms; GPU upload " << stats.upload / 60 << " + rays " << stats.draw / 60 << " + read back " << stats.read / 60 << " ms; "
			<< stats.differentRays << " of " << stats.checkedRays << " rays checked differ, " << stats.differentPixels << " of " << stats.checkedPixels << " colours";
		Log(LOG_INFO) << line.str();
		const int detailed = stats.detailed;
		stats = ShadowStats();
		stats.detailed = detailed;
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
